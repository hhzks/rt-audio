#include "ffi/Session.h"
#include "ffi/Utf8.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <utility>

namespace rt {

static_assert(RT_HIST_BUCKETS == RtHistogram::kBucketCount);
static_assert(RT_MAX_CHANNELS == kMaxChannels);
static_assert(RT_TAPER_LINEAR == static_cast<int>(Taper::Linear));
static_assert(RT_TAPER_LOG == static_cast<int>(Taper::Log));
static_assert(RT_FLAG_READ_ONLY == kReadOnly && RT_FLAG_TOGGLE == kToggle);

namespace {

constexpr std::array<ParamInfo, 3> kMasterInfo{{
    {"in_gain",  "in",     "dB", -24.0, 24.0, 0.0, Taper::Linear, 0},
    {"out_gain", "out",    "dB", -60.0, 12.0, 0.0, Taper::Linear, 0},
    {"bypass",   "bypass", "",     0.0,  1.0, 0.0, Taper::Linear, kToggle},
}};

} // namespace

Session::Session() : Session(DeviceMaker([](Backend b) { return createAudioDevice(b); })) {}

Session::Session(DeviceMaker make) : master_(kMasterInfo), make_(std::move(make)) {
    rig_ = buildRigChain(engine_.chain());
    applyMaster();
}

Session::~Session() { stop(); }

void Session::checkOpened() const {
    if (!opened_) throw SessionStateError("session is not open");
}

std::unique_ptr<IAudioDevice> Session::makeAndStart(const DeviceConfig& config) {
    auto device = make_(backend_);
    if (!device) throw std::runtime_error("device factory returned no device");
    device->open(config, &callback_);
    const DeviceStatus st = device->status();
    engine_.prepare(st.sampleRate, st.blockFrames, st.numChannels);
    device->start();
    return device;
}

std::optional<std::string> Session::tryStart(const DeviceConfig& config) {
    try {
        device_ = makeAndStart(config);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        return std::string(e.what());
    } catch (...) {
        return std::string("unknown error");
    }
    return std::nullopt;
}

void Session::destroyDevice() noexcept {
    if (!device_) return;
    try { device_->stop(); } catch (...) {}
    try { device_->close(); } catch (...) {}
    device_.reset();
}

void Session::open(Backend backend, const DeviceConfig& config) {
    if (opened_) throw SessionStateError("session is already open");
    backend_ = resolveBackend(backend);
    device_  = makeAndStart(config);
    config_  = config;
    opened_  = true;
}

ReconfigureResult Session::reconfigure(const DeviceConfig& next) {
    checkOpened();
    destroyDevice();

    const auto e1 = tryStart(next);
    if (!e1) {
        config_ = next;
        stopReason_.clear();
        message_.clear();
        return ReconfigureResult::Applied;
    }
    if (next == config_) {
        stopReason_ = message_ = "could not reopen the device: " + *e1;
        return ReconfigureResult::Stopped;
    }
    const std::string first = "could not open the new config: " + *e1;
    const auto e2 = tryStart(config_);
    if (!e2) {
        stopReason_.clear();
        message_ = first + "; restored the previous config";
        return ReconfigureResult::RolledBack;
    }
    stopReason_ = message_ = first + "; could not restore the previous config: " + *e2;
    return ReconfigureResult::Stopped;
}

std::vector<DeviceInfo> Session::enumerate() {
    checkOpened();
    auto probe = make_(backend_);
    if (!probe) throw std::runtime_error("device factory returned no device");
    return probe->enumerate();
}

void Session::stop() noexcept {
    if (!device_) return;
    try { device_->stop(); } catch (...) {}
}

const DeviceConfig& Session::config() const {
    checkOpened();
    return config_;
}

Backend Session::backend() const {
    checkOpened();
    return backend_;
}

void Session::latencyEnter() {
    checkOpened();
    latencyMode_.store(true, std::memory_order_relaxed);
    callback_.setMode(CallbackMode::Silent);
}

void Session::latencyLeave() {
    checkOpened();
    latencyMode_.store(false, std::memory_order_relaxed);
    callback_.setMode(CallbackMode::Normal);
}

std::size_t Session::stripCount() const noexcept { return 1 + engine_.chain().size(); }

const IEffect& Session::effect(std::size_t strip) const {
    if (strip == 0 || strip >= stripCount()) throw SessionArgError("strip index out of range");
    return *engine_.chain().at(strip - 1);
}

const char* Session::stripName(std::size_t strip) const {
    return strip == 0 ? "Master" : effect(strip).name();
}

std::span<const ParamInfo> Session::stripParams(std::size_t strip) const {
    return strip == 0 ? master_.info() : effect(strip).params();
}

FrameCount Session::stripLatency(std::size_t strip) const {
    return strip == 0 ? 0 : effect(strip).latencyFrames();
}

void Session::checkParam(std::size_t strip, std::size_t param) const {
    if (param >= stripParams(strip).size()) throw SessionArgError("parameter index out of range");
}

double Session::paramDefault(std::size_t strip, std::size_t param) const {
    checkParam(strip, param);
    return strip == 0 ? master_.defaultValue(param) : effect(strip).paramDefault(param);
}

double Session::getParam(std::size_t strip, std::size_t param) const {
    checkParam(strip, param);
    return strip == 0 ? master_.get(param) : effect(strip).getParam(param);
}

void Session::setParam(std::size_t strip, std::size_t param, double value) {
    checkParam(strip, param);
    if (strip == 0) {
        if (!master_.set(param, value)) throw SessionArgError("value rejected");
        applyMaster();
        return;
    }
    if (!engine_.chain().at(strip - 1)->setParam(param, value))
        throw SessionArgError("value rejected");
}

void Session::applyMaster() noexcept {
    ParameterStore& p = engine_.params();
    p.inputGain.store(static_cast<float>(std::pow(10.0, master_.get(kInGain) / 20.0)),
                      std::memory_order_relaxed);
    p.outputGain.store(static_cast<float>(std::pow(10.0, master_.get(kOutGain) / 20.0)),
                       std::memory_order_relaxed);
    p.bypass.store(master_.get(kBypass) >= 0.5, std::memory_order_relaxed);
}

DeviceStatus Session::deviceStatus() const {
    checkOpened();
    if (!device_) throw SessionStateError("no device is open: " + stopReason_);
    return device_->status();
}

void Session::snapshot(rt_snapshot& out) {
    checkOpened();
    out = rt_snapshot{};

    RtStats& s = engine_.stats();
    const DeviceStatus ds = device_ ? device_->status() : DeviceStatus{};
    out.callbacks         = s.callbackCount.load(std::memory_order_relaxed);
    out.engine_xruns      = s.xruns.load(std::memory_order_relaxed);
    out.device_xruns      = ds.xruns;
    out.capture_overruns  = ds.captureOverruns;
    out.capture_underruns = ds.captureUnderruns;
    out.in_clips          = s.inputClips.load(std::memory_order_relaxed);
    out.out_clips         = s.outputClips.load(std::memory_order_relaxed);
    out.deadline_ns       = s.blockDeadlineNanos.load(std::memory_order_relaxed);

    const HistogramSnapshot h = s.callbackNanos.drain();
    for (std::size_t b = 0; b < h.counts.size(); ++b) out.hist_window[b] = h.counts[b];

    const int channels = std::min(engine_.numChannels(), static_cast<int>(RT_MAX_CHANNELS));
    out.channels = channels;
    for (int c = 0; c < channels; ++c) {
        out.in_peak[c]  = s.inputPeak[idx(c)].exchange(0.0f, std::memory_order_relaxed);
        out.out_peak[c] = s.outputPeak[idx(c)].exchange(0.0f, std::memory_order_relaxed);
    }

    const std::size_t strips = std::min<std::size_t>(stripCount(), RT_MAX_STRIPS);
    for (std::size_t st = 0; st < strips; ++st) {
        const std::size_t n = std::min<std::size_t>(stripParams(st).size(), RT_MAX_PARAMS);
        for (std::size_t p = 0; p < n; ++p) out.params[st][p] = getParam(st, p);
    }

    out.running = static_cast<std::uint8_t>(device_ && device_->isRunning() ? 1 : 0);
    copyUtf8Truncated(out.device_error, sizeof out.device_error,
                      device_ ? ds.lastError : stopReason_);
}

} // namespace rt
