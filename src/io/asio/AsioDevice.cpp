#include "io/asio/AsioDevice.h"
#include "io/WinString.h"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace rt {

static_assert(static_cast<long>(AsioSampleType::Int16MSB) == ASIOSTInt16MSB);
static_assert(static_cast<long>(AsioSampleType::Int32MSB) == ASIOSTInt32MSB);
static_assert(static_cast<long>(AsioSampleType::Float32MSB) == ASIOSTFloat32MSB);
static_assert(static_cast<long>(AsioSampleType::Int16LSB) == ASIOSTInt16LSB);
static_assert(static_cast<long>(AsioSampleType::Int24LSB) == ASIOSTInt24LSB);
static_assert(static_cast<long>(AsioSampleType::Int32LSB) == ASIOSTInt32LSB);
static_assert(static_cast<long>(AsioSampleType::Float32LSB) == ASIOSTFloat32LSB);
static_assert(static_cast<long>(AsioSampleType::Float64LSB) == ASIOSTFloat64LSB);
static_assert(static_cast<long>(AsioSampleType::Int32LSB16) == ASIOSTInt32LSB16);
static_assert(static_cast<long>(AsioSampleType::Int32LSB18) == ASIOSTInt32LSB18);
static_assert(static_cast<long>(AsioSampleType::Int32LSB20) == ASIOSTInt32LSB20);
static_assert(static_cast<long>(AsioSampleType::Int32LSB24) == ASIOSTInt32LSB24);
static_assert(static_cast<long>(AsioSampleType::DSDInt8LSB1) == ASIOSTDSDInt8LSB1);

static_assert(static_cast<long>(AsioSelector::SelectorSupported) == kAsioSelectorSupported);
static_assert(static_cast<long>(AsioSelector::EngineVersion) == kAsioEngineVersion);
static_assert(static_cast<long>(AsioSelector::ResetRequest) == kAsioResetRequest);
static_assert(static_cast<long>(AsioSelector::BufferSizeChange) == kAsioBufferSizeChange);
static_assert(static_cast<long>(AsioSelector::ResyncRequest) == kAsioResyncRequest);
static_assert(static_cast<long>(AsioSelector::LatenciesChanged) == kAsioLatenciesChanged);
static_assert(static_cast<long>(AsioSelector::SupportsTimeInfo) == kAsioSupportsTimeInfo);
static_assert(static_cast<long>(AsioSelector::Overload) == kAsioOverload);

namespace {

void check(ASIOError e, const char* what) {
    if (e != ASE_OK) throw std::runtime_error(std::format("ASIO {} failed (error {})", what, e));
}

std::wstring readString(HKEY root, const wchar_t* subkey, const wchar_t* value) {
    wchar_t buf[512];
    DWORD size = sizeof buf;
    if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

bool hasInprocServer(const std::wstring& clsid) {
    const std::wstring path = L"CLSID\\" + clsid + L"\\InprocServer32";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    RegCloseKey(key);
    return true;
}

IASIO* createRegisteredDriver(const std::string& id) {
    const std::wstring key = L"SOFTWARE\\ASIO\\" + utf8ToWide(id);
    const std::wstring text = readString(HKEY_LOCAL_MACHINE, key.c_str(), L"CLSID");
    if (text.empty()) throw std::runtime_error("no ASIO driver named \"" + id + "\"");
    CLSID clsid{};
    if (FAILED(CLSIDFromString(text.c_str(), &clsid)))
        throw std::runtime_error("the ASIO driver \"" + id + "\" has an invalid CLSID");
    IASIO* driver = nullptr;
    const HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid,
                                        reinterpret_cast<void**>(&driver));
    if (FAILED(hr))
        throw std::runtime_error(std::format("could not load the ASIO driver \"{}\" (hr=0x{:08x})",
                                             id, static_cast<unsigned>(hr)));
    return driver;
}

} // namespace

std::atomic<AsioDevice*> AsioDevice::active_{nullptr};
ASIOCallbacks AsioDevice::callbacks_{&AsioDevice::onBufferSwitch, &AsioDevice::onSampleRateChanged,
                                     &AsioDevice::onAsioMessage, &AsioDevice::onBufferSwitchTimeInfo};

AsioDevice::AsioDevice() : AsioDevice(createRegisteredDriver) {}

AsioDevice::AsioDevice(DriverFactory factory) : factory_(std::move(factory)) {}

AsioDevice::~AsioDevice() {
    try { close(); } catch (...) {}
    host_.reset();
}

std::vector<DeviceInfo> AsioDevice::enumerate() {
    std::vector<DeviceInfo> out;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD len = 256;
        if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        const std::wstring clsid = readString(root, name, L"CLSID");
        if (clsid.empty() || !hasInprocServer(clsid)) continue;
        const std::wstring description = readString(root, name, L"Description");
        DeviceInfo d;
        d.id = wideToUtf8(name);
        d.name = description.empty() ? d.id : wideToUtf8(description.c_str());
        d.maxInputChannels  = 2;
        d.maxOutputChannels = 2;
        out.push_back(std::move(d));
    }
    RegCloseKey(root);
    return out;
}

std::string AsioDevice::driverId(const DeviceConfig& config) {
    if (!config.inputId.empty() && !config.outputId.empty() && config.inputId != config.outputId)
        throw std::invalid_argument("ASIO uses one driver for input and output");
    if (!config.inputId.empty()) return config.inputId;
    if (!config.outputId.empty()) return config.outputId;
    const auto drivers = enumerate();
    if (drivers.empty()) throw std::runtime_error("no ASIO driver is installed");
    return drivers.front().id;
}

void AsioDevice::open(const DeviceConfig& config, IAudioCallback* callback) {
    if (!callback) throw std::invalid_argument("AsioDevice::open: null callback");
    if (!validRingBlocks(config.ringBlocks))
        throw std::invalid_argument("AsioDevice::open: ringBlocks outside 1.0..2.0");
    if (config.numChannels < 1 || config.numChannels > kMaxChannels)
        throw std::invalid_argument("AsioDevice::open: numChannels outside 1..8");
    close();
    const std::string id = driverId(config);
    if (!host_) host_ = std::make_unique<ComHostThread>();
    host_->run([&] { openOnHost(id, config, callback); });
}

void AsioDevice::openOnHost(const std::string& id, const DeviceConfig& config,
                            IAudioCallback* callback) {
    AsioDevice* none = nullptr;
    if (!active_.compare_exchange_strong(none, this))
        throw std::runtime_error("another ASIO device is open in this process");
    try {
        driver_ = factory_(id);
        if (!driver_) throw std::runtime_error("could not load the ASIO driver \"" + id + "\"");
        if (driver_->init(host_->window()) == ASIOFalse) {
            char message[128] = {};
            driver_->getErrorMessage(message);
            throw std::runtime_error(std::format("the ASIO driver \"{}\" did not start: {}", id, message));
        }
        long ins = 0, outs = 0;
        check(driver_->getChannels(&ins, &outs), "getChannels");
        if (ins < 1 || outs < 1)
            throw std::runtime_error("the ASIO driver needs at least one input and one output");
        if (driver_->canSampleRate(config.sampleRate) != ASE_OK)
            throw std::runtime_error(std::format("the ASIO driver does not support {} Hz", config.sampleRate));
        check(driver_->setSampleRate(config.sampleRate), "setSampleRate");

        long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
        check(driver_->getBufferSize(&minSize, &maxSize, &preferred, &granularity), "getBufferSize");
        const long block = chooseAsioBufferSize(config.blockFrames, minSize, maxSize, preferred, granularity);

        inputs_  = static_cast<int>(std::min(ins, 2L));
        outputs_ = static_cast<int>(std::min(outs, 2L));
        buffers_.assign(idx(inputs_ + outputs_), ASIOBufferInfo{});
        for (int i = 0; i < inputs_ + outputs_; ++i) {
            buffers_[idx(i)].isInput    = i < inputs_ ? ASIOTrue : ASIOFalse;
            buffers_[idx(i)].channelNum = i < inputs_ ? i : i - inputs_;
        }
        check(driver_->createBuffers(buffers_.data(), static_cast<long>(buffers_.size()), block, &callbacks_),
              "createBuffers");
        buffersCreated_ = true;

        types_.clear();
        for (const ASIOBufferInfo& b : buffers_) {
            ASIOChannelInfo info{};
            info.channel = b.channelNum;
            info.isInput = b.isInput;
            check(driver_->getChannelInfo(&info), "getChannelInfo");
            const auto type = static_cast<AsioSampleType>(info.type);
            if (!asioTypeSupported(type))
                throw std::runtime_error(std::format("the ASIO driver uses an unsupported sample type ({})",
                                                     asioTypeName(type)));
            types_.push_back(type);
        }

        long inLatency = 0, outLatency = 0;
        check(driver_->getLatencies(&inLatency, &outLatency), "getLatencies");
        useOutputReady_ = driver_->outputReady() == ASE_OK;

        char name[33] = {};
        driver_->getDriverName(name);

        config_   = config;
        callback_ = callback;
        block_    = static_cast<FrameCount>(block);
        engineIn_.assign(idx(block_) * idx(config.numChannels), 0.0f);
        engineOut_.assign(engineIn_.size(), 0.0f);
        xruns_.store(0, std::memory_order_relaxed);
        {
            std::lock_guard lock(statusMutex_);
            status_ = DeviceStatus{};
            status_.sampleRate  = config.sampleRate;
            status_.blockFrames = block_;
            status_.numChannels = config.numChannels;
            status_.estimatedRoundTripMs =
                1000.0 * static_cast<double>(inLatency + outLatency) / config.sampleRate;
            status_.backendName = std::format("ASIO® ({}) in {} out {}", name,
                                              asioTypeName(types_.front()),
                                              asioTypeName(types_[idx(inputs_)]));
            status_.inputName = status_.outputName = name;
        }
        processing_.store(true, std::memory_order_release);
    } catch (...) {
        releaseOnHost();
        throw;
    }
}

void AsioDevice::start() {
    if (!host_) throw std::runtime_error("AsioDevice::start: not open");
    host_->run([this] {
        if (!driver_) throw std::runtime_error("AsioDevice::start: not open");
        if (running_.load()) return;
        check(driver_->start(), "start");
        running_.store(true, std::memory_order_release);
    });
}

void AsioDevice::stop() {
    if (!host_) return;
    host_->run([this] {
        if (driver_ && running_.load()) driver_->stop();
        running_.store(false, std::memory_order_release);
    });
}

void AsioDevice::close() {
    if (!host_) return;
    host_->run([this] { releaseOnHost(); });
}

DeviceStatus AsioDevice::status() const {
    std::lock_guard lock(statusMutex_);
    DeviceStatus s = status_;
    s.xruns = xruns_.load(std::memory_order_relaxed);
    return s;
}

void AsioDevice::releaseOnHost() noexcept {
    processing_.store(false, std::memory_order_release);
    if (driver_) {
        if (running_.load()) driver_->stop();
        if (buffersCreated_) driver_->disposeBuffers();
        driver_->Release();
    }
    driver_ = nullptr;
    buffersCreated_ = false;
    running_.store(false, std::memory_order_release);
    AsioDevice* self = this;
    active_.compare_exchange_strong(self, nullptr);
}

void AsioDevice::requestReset() noexcept {
    if (resetPending_.exchange(true)) return;
    try {
        host_->post([this] { resetOnHost(); });
    } catch (...) {
        resetPending_.store(false);
    }
}

void AsioDevice::resetOnHost() noexcept {
    resetPending_.store(false);
    if (!driver_) return;
    {
        std::lock_guard lock(statusMutex_);
        status_.lastError = "the ASIO driver requested a reset";
    }
    releaseOnHost();
}

void AsioDevice::refreshLatencies() noexcept {
    if (!driver_) return;
    long in = 0, out = 0;
    if (driver_->getLatencies(&in, &out) != ASE_OK) return;
    std::lock_guard lock(statusMutex_);
    status_.estimatedRoundTripMs = 1000.0 * static_cast<double>(in + out) / config_.sampleRate;
}

void AsioDevice::process(long index) noexcept {
    if (!processing_.load(std::memory_order_acquire)) return;
    const int ch = config_.numChannels;
    for (int c = 0; c < ch; ++c) {
        const int in = std::min(c, inputs_ - 1);
        asioToFloat(buffers_[idx(in)].buffers[index], types_[idx(in)], engineIn_.data() + c, ch, block_);
    }
    callback_->audioDeviceProcess(engineIn_.data(), engineOut_.data(), block_);
    for (int o = 0; o < outputs_; ++o) {
        const int b = inputs_ + o;
        floatToAsio(engineOut_.data() + std::min(o, ch - 1), ch, buffers_[idx(b)].buffers[index],
                    types_[idx(b)], block_);
    }
    if (useOutputReady_) driver_->outputReady();
}

void AsioDevice::onBufferSwitch(long index, ASIOBool) {
    if (AsioDevice* d = active_.load(std::memory_order_acquire)) d->process(index);
}

ASIOTime* AsioDevice::onBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool) {
    if (AsioDevice* d = active_.load(std::memory_order_acquire)) d->process(index);
    return params;
}

void AsioDevice::onSampleRateChanged(ASIOSampleRate) {
    if (AsioDevice* d = active_.load(std::memory_order_acquire)) d->requestReset();
}

long AsioDevice::onAsioMessage(long selector, long value, void*, double*) {
    const AsioMessageReply r = handleAsioMessage(selector, value);
    AsioDevice* d = active_.load(std::memory_order_acquire);
    if (!d) return r.reply;
    switch (r.action) {
    case AsioAction::Reset:
        d->requestReset();
        break;
    case AsioAction::Xrun:
        d->xruns_.fetch_add(1, std::memory_order_relaxed);
        break;
    case AsioAction::LatenciesChanged:
        d->host_->post([d] { d->refreshLatencies(); });
        break;
    case AsioAction::None:
        break;
    }
    return r.reply;
}

} // namespace rt
