#ifdef _WIN32
#include "io/wasapi/WasapiDevice.h"
#include "io/wasapi/MmcssScope.h"

#include "core/ChannelMap.h"
#include "core/RingPush.h"

#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <stdexcept>

namespace rt {

static_assert(kHrDeviceInvalidated == static_cast<std::int32_t>(AUDCLNT_E_DEVICE_INVALIDATED));
static_assert(kHrServiceNotRunning == static_cast<std::int32_t>(AUDCLNT_E_SERVICE_NOT_RUNNING));

namespace {

void throwIfFailed(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        std::ostringstream os;
        os << what << " failed, hr=0x" << std::hex << static_cast<unsigned>(hr);
        if (hr == AUDCLNT_E_DEVICE_IN_USE)
            os << " (device already open in exclusive mode by another app)";
        if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT)
            os << " (requested format not supported -- try the device's mix format)";
        throw std::runtime_error(os.str());
    }
}

std::string wideToUtf8(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::optional<SampleFormat> detectFormat(const WAVEFORMATEX* fmt) noexcept {
    const bool ext = fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE;
    GUID sub{};
    if (ext) sub = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt)->SubFormat;

    const bool isFloat = ext ? (sub == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
                             : (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    const bool isPcm   = ext ? (sub == KSDATAFORMAT_SUBTYPE_PCM)
                             : (fmt->wFormatTag == WAVE_FORMAT_PCM);

    if (isFloat && fmt->wBitsPerSample == 32) return SampleFormat::Float32;
    if (isPcm) {
        switch (fmt->wBitsPerSample) {
            case 16: return SampleFormat::Int16;
            case 24: return SampleFormat::Int24;
            case 32: return SampleFormat::Int32;
            default: break;
        }
    }
    return std::nullopt;
}

const char* formatName(SampleFormat f) noexcept {
    switch (f) {
        case SampleFormat::Int16:   return "16-bit PCM";
        case SampleFormat::Int24:   return "24-bit PCM";
        case SampleFormat::Int32:   return "32-bit PCM";
        case SampleFormat::Float32: return "32-bit float";
    }
    return "?";
}

WAVEFORMATEXTENSIBLE* allocFormat(WORD channels, DWORD rate, WORD bits, bool isFloat) noexcept {
    auto* w = static_cast<WAVEFORMATEXTENSIBLE*>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
    if (!w) return nullptr;
    std::memset(w, 0, sizeof(*w));
    w->Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    w->Format.nChannels       = channels;
    w->Format.nSamplesPerSec  = rate;
    w->Format.wBitsPerSample  = bits;
    w->Format.nBlockAlign     = static_cast<WORD>(channels * (bits / 8));
    w->Format.nAvgBytesPerSec = static_cast<DWORD>(rate * w->Format.nBlockAlign);
    w->Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w->Samples.wValidBitsPerSample = bits;
    w->dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER
                     : channels == 2 ? static_cast<DWORD>(SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                                     : static_cast<DWORD>((1u << channels) - 1u);
    w->SubFormat = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    return w;
}

} // namespace

// ---------------------------------------------------------------- Endpoint --

void WasapiDevice::Endpoint::release() {
    if (format) { CoTaskMemFree(format); format = nullptr; }
    if (event)  { CloseHandle(event); event = nullptr; }
    client.reset();
    device.reset();
}

// ------------------------------------------------------------ construction --

WasapiDevice::WasapiDevice() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // RPC_E_CHANGED_MODE means someone already picked STA. We can still work,
    // but do not uninitialise what we did not initialise.
    comInitialized_ = SUCCEEDED(hr);
    if (hr != RPC_E_CHANGED_MODE) throwIfFailed(hr, "CoInitializeEx");

    throwIfFailed(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), enumerator_.putVoid()),
                  "CoCreateInstance(MMDeviceEnumerator)");
}

WasapiDevice::~WasapiDevice() {
    close();
    enumerator_.reset();
    if (comInitialized_) CoUninitialize();
}

// ------------------------------------------------------------- enumeration --

std::vector<DeviceInfo> WasapiDevice::enumerate() {
    std::vector<DeviceInfo> result;

    for (EDataFlow flow : { eCapture, eRender }) {
        ComPtr<IMMDeviceCollection> collection;
        throwIfFailed(enumerator_->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put()),
                      "EnumAudioEndpoints");

        ComPtr<IMMDevice> defaultDevice;
        LPWSTR defaultId = nullptr;
        if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(flow, eConsole, defaultDevice.put())))
            defaultDevice->GetId(&defaultId);

        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> dev;
            if (FAILED(collection->Item(i, dev.put()))) continue;

            LPWSTR id = nullptr;
            dev->GetId(&id);

            ComPtr<IPropertyStore> props;
            std::string friendly = "(unnamed)";
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put()))) {
                PROPVARIANT pv; PropVariantInit(&pv);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) &&
                    pv.vt == VT_LPWSTR)
                    friendly = wideToUtf8(pv.pwszVal);
                PropVariantClear(&pv);
            }

            DeviceInfo info;
            info.id   = wideToUtf8(id);
            info.name = friendly;

            ComPtr<IAudioClient> client;
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                        client.putVoid())) &&
                SUCCEEDED(client->GetMixFormat(&mix))) {
                info.defaultSampleRate = mix->nSamplesPerSec;
                (flow == eCapture ? info.maxInputChannels : info.maxOutputChannels) =
                    mix->nChannels;
                CoTaskMemFree(mix);
            }

            const bool isDefault = defaultId && id && wcscmp(defaultId, id) == 0;
            if (flow == eCapture) info.isDefaultInput = isDefault;
            else                  info.isDefaultOutput = isDefault;

            if (id) CoTaskMemFree(id);
            result.push_back(std::move(info));
        }
        if (defaultId) CoTaskMemFree(defaultId);
    }
    return result;
}

// --------------------------------------------------------------- endpoints --

void WasapiDevice::initEndpoint(Endpoint& ep, EDataFlow flow, const std::string& id,
                                FrameCount requestedFrames, bool exclusive) {
    if (id.empty()) {
        throwIfFailed(enumerator_->GetDefaultAudioEndpoint(flow, eConsole, ep.device.put()),
                      "GetDefaultAudioEndpoint");
    } else {
        const std::wstring wid = utf8ToWide(id);
        throwIfFailed(enumerator_->GetDevice(wid.c_str(), ep.device.put()), "GetDevice");
    }

    throwIfFailed(ep.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                      ep.client.putVoid()),
                  "IMMDevice::Activate(IAudioClient)");

    throwIfFailed(ep.client->GetMixFormat(&ep.format), "GetMixFormat");

    // GetMixFormat describes SHARED mode. Exclusive endpoints often reject it.
    if (exclusive &&
        FAILED(ep.client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, ep.format, nullptr))) {
        const WORD  chans = ep.format->nChannels;
        const DWORD rate  = ep.format->nSamplesPerSec;
        const struct { WORD bits; bool isFloat; } depths[] = {
            { 32, true }, { 32, false }, { 24, false }, { 16, false },
        };
        // Channel count is negotiable too: capture endpoints are often mono-only
        // in exclusive mode even when their mix format is stereo.
        const WORD channelCandidates[] = { chans, 2, 1 };
        bool found = false;
        for (WORD c : channelCandidates) {
            if (c == 0) continue;
            for (const auto& d : depths) {
                auto* w = allocFormat(c, rate, d.bits, d.isFloat);
                if (!w) continue;
                if (SUCCEEDED(ep.client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                           &w->Format, nullptr))) {
                    CoTaskMemFree(ep.format);
                    ep.format = &w->Format;
                    found = true;
                    break;
                }
                CoTaskMemFree(w);
            }
            if (found) break;
        }
        if (!found)
            throw std::runtime_error("endpoint accepted no exclusive-mode format "
                                     "(tried 32f, 32, 24, 16-bit at the mix rate)");
    }

    const auto detected = detectFormat(ep.format);
    if (!detected)
        throw std::runtime_error("endpoint format is not 16/24/32-bit PCM or 32-bit float");
    ep.sampleFormat = *detected;
    ep.channels     = ep.format->nChannels;

    bool initialized = false;

    if (!exclusive) {
        // Preferred path: IAudioClient3 lets us ask for periods well below the
        // default 10 ms -- often 128 frames (2.67 ms at 48 kHz) if the driver
        // cooperates. Available since Windows 10 1607.
        ComPtr<IAudioClient3> client3;
        if (SUCCEEDED(ep.client->QueryInterface(__uuidof(IAudioClient3), client3.putVoid()))) {
            UINT32 defaultPeriod = 0, fundamental = 0, minPeriod = 0, maxPeriod = 0;
            if (SUCCEEDED(client3->GetSharedModeEnginePeriod(ep.format, &defaultPeriod,
                                                             &fundamental, &minPeriod,
                                                             &maxPeriod))) {
                UINT32 period = minPeriod;
                if (requestedFrames > 0) {
                    period = std::clamp(static_cast<UINT32>(requestedFrames), minPeriod, maxPeriod);
                    // Must be a multiple of the fundamental period.
                    if (fundamental > 0) period = (period / fundamental) * fundamental;
                    period = std::max(period, minPeriod);
                }
                if (SUCCEEDED(client3->InitializeSharedAudioStream(
                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, ep.format, nullptr)))
                    initialized = true;
            }
        }
    }

    if (!initialized) {
        // Fallback: classic Initialize. Buffer duration 0 asks the engine for
        // its minimum in shared mode. In exclusive mode we must pass the
        // device's own default period.
        REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
        ep.client->GetDevicePeriod(&defaultPeriod, &minPeriod);
        const REFERENCE_TIME duration = exclusive ? minPeriod : 0;

        HRESULT hr = ep.client->Initialize(
            exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration,
            exclusive ? duration : 0, ep.format, nullptr);

        // Exclusive mode commonly fails once with a buffer-alignment error and
        // wants to be retried at the size it tells you. This is expected, not
        // a bug -- see the AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED docs.
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 alignedFrames = 0;
            ep.client->GetBufferSize(&alignedFrames);
            ep.client.reset();
            throwIfFailed(ep.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                              ep.client.putVoid()), "re-Activate");
            const auto aligned = static_cast<REFERENCE_TIME>(
                10000.0 * 1000 * alignedFrames / ep.format->nSamplesPerSec + 0.5);
            hr = ep.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                       AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                       aligned, aligned, ep.format, nullptr);
        }
        throwIfFailed(hr, "IAudioClient::Initialize");
    }

    ep.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ep.event) throw std::runtime_error("CreateEvent failed");
    throwIfFailed(ep.client->SetEventHandle(ep.event), "SetEventHandle");
    throwIfFailed(ep.client->GetBufferSize(&ep.bufferFrames), "GetBufferSize");
}

// -------------------------------------------------------------------- open --

void WasapiDevice::open(const DeviceConfig& config, IAudioCallback* callback) {
    if (!callback) throw std::invalid_argument("WasapiDevice::open: null callback");
    close();

    config_   = config;
    callback_ = callback;

    initEndpoint(capture_, eCapture, config.inputId,  config.blockFrames, config.exclusiveMode);
    initEndpoint(render_,  eRender,  config.outputId, config.blockFrames, config.exclusiveMode);

    throwIfFailed(capture_.client->GetService(__uuidof(IAudioCaptureClient),
                                             captureService_.putVoid()),
                  "GetService(IAudioCaptureClient)");
    throwIfFailed(render_.client->GetService(__uuidof(IAudioRenderClient),
                                            renderService_.putVoid()),
                  "GetService(IAudioRenderClient)");

    const int    engineCh = config.numChannels;
    const double sr       = render_.format->nSamplesPerSec;
    const auto   maxBlock = static_cast<std::size_t>(
        std::max(capture_.bufferFrames, render_.bufferFrames));

    // Capture frames consumed per render frame. The two endpoints are separate
    // crystals, so this is never exactly right for long -- drift_ trims it.
    nominalRatio_ = static_cast<double>(capture_.format->nSamplesPerSec) / sr;

    // Ring holds ~4 render buffers' worth. Too small and normal jitter causes
    // dropouts; too large and you have added pure latency for nothing.
    captureRing_.reset(maxBlock * idx(engineCh) * 4);

    resampler_.prepare(engineCh, nominalRatio_);
    ringTargetFrames_ = captureRing_.capacity() / 2 / idx(engineCh);

    engineIn_.assign(maxBlock * idx(engineCh), 0.0f);
    engineOut_.assign(maxBlock * idx(engineCh), 0.0f);
    convertScratch_.assign(maxBlock * idx(std::max(capture_.channels, render_.channels)), 0.0f);
    deviceScratch_.assign(maxBlock * idx(std::max(capture_.channels, render_.channels)), 0.0f);

    // Resampling 44.1 -> 48 makes a packet LONGER than it arrived, so this one
    // cannot be sized off maxBlock like the others.
    resampleScratch_.assign(
        idx(AsyncResampler::maxOutputFor(static_cast<FrameCount>(maxBlock), nominalRatio_))
            * idx(engineCh), 0.0f);

    status_.sampleRate  = sr;
    status_.blockFrames = static_cast<FrameCount>(render_.bufferFrames);
    status_.numChannels = engineCh;
    status_.backendName = std::string(config.exclusiveMode ? "WASAPI (exclusive)"
                                                          : "WASAPI (shared)")
                        + " in " + formatName(capture_.sampleFormat)
                        + " out " + formatName(render_.sampleFormat);
    status_.estimatedRoundTripMs =
        1000.0 * (capture_.bufferFrames + render_.bufferFrames) / sr
      + 1000.0 * AsyncResampler::latencyFrames()
            / static_cast<double>(capture_.format->nSamplesPerSec);

    if (capture_.format->nSamplesPerSec != render_.format->nSamplesPerSec)
        status_.backendName += " asrc " + std::to_string(capture_.format->nSamplesPerSec)
                             + "->" + std::to_string(render_.format->nSamplesPerSec);

    shutdownEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

// --------------------------------------------------------- transport + RT --

void WasapiDevice::start() {
    if (running_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();   // a thread that stopped on a fatal error
    lastHr_.store(0, std::memory_order_relaxed);

    const std::size_t ch = idx(config_.numChannels);
    primeRing(captureRing_, ringTargetFrames_, ch);
    resampler_.reset();
    drift_.prepare(ringTargetFrames_ * ch, 0.002);

    ResetEvent(shutdownEvent_);
    thread_ = std::thread(&WasapiDevice::threadMain, this);
}

void WasapiDevice::stop() {
    running_.store(false, std::memory_order_release);
    if (shutdownEvent_) SetEvent(shutdownEvent_);
    if (thread_.joinable()) thread_.join();
}

void WasapiDevice::close() {
    stop();
    captureService_.reset();
    renderService_.reset();
    capture_.release();
    render_.release();
    if (shutdownEvent_) { CloseHandle(shutdownEvent_); shutdownEvent_ = nullptr; }
    callback_ = nullptr;
}

DeviceStatus WasapiDevice::status() const {
    DeviceStatus s = status_;
    s.captureOverruns = captureOverruns_.load(std::memory_order_relaxed);
    s.captureUnderruns = captureUnderruns_.load(std::memory_order_relaxed);
    s.xruns = xruns_.load(std::memory_order_relaxed);
    const long hr = lastHr_.load(std::memory_order_relaxed);
    if (hr != 0) {
        std::ostringstream os;
        os << "WASAPI device lost (hr=0x" << std::hex << static_cast<unsigned long>(hr) << ")";
        s.lastError = os.str();
    }
    return s;
}

bool WasapiDevice::fatal(HRESULT hr) noexcept {
    if (wasapiActionFor(static_cast<std::int32_t>(hr)) != WasapiAction::Fail) return false;
    lastHr_.store(hr, std::memory_order_relaxed);
    return true;
}

bool WasapiDevice::drainCapture() noexcept {
    for (;;) {
        UINT32 packetFrames = 0;
        const HRESULT hrSize = captureService_->GetNextPacketSize(&packetFrames);
        if (FAILED(hrSize)) return !fatal(hrSize);
        if (packetFrames == 0) return true;

        BYTE*  data  = nullptr;
        UINT32 frames = 0;
        DWORD  flags  = 0;
        const HRESULT hrGet = captureService_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hrGet)) return !fatal(hrGet);

        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
            xruns_.fetch_add(1, std::memory_order_relaxed);

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            std::memset(convertScratch_.data(), 0,
                        sizeof(float) * static_cast<std::size_t>(frames) * idx(config_.numChannels));
        } else {
            toFloat(data, deviceScratch_.data(),
                    static_cast<std::size_t>(frames) * idx(capture_.channels),
                    capture_.sampleFormat);
            remapChannels(deviceScratch_.data(), capture_.channels, convertScratch_.data(),
                          config_.numChannels, static_cast<FrameCount>(frames));
        }

        const std::size_t ch = static_cast<std::size_t>(config_.numChannels);

        // Nominal rate difference plus the drift trim, recomputed per packet.
        // Ring fill is the only observable that says which crystal runs fast.
        resampler_.setRatio(nominalRatio_ * drift_.update(captureRing_.readAvailable()));

        const FrameCount produced =
            resampler_.process(convertScratch_.data(), static_cast<FrameCount>(frames),
                               resampleScratch_.data(),
                               static_cast<FrameCount>(resampleScratch_.size() / ch));

        if (pushEvictingOldest(captureRing_, resampleScratch_.data(), idx(produced) * ch, ch))
            captureOverruns_.fetch_add(1, std::memory_order_relaxed);
        captureService_->ReleaseBuffer(frames);
    }
}

bool WasapiDevice::fillRender() noexcept {
    const RenderFrames rf = wasapiRenderFrames(
        config_.exclusiveMode, render_.bufferFrames, [this](std::uint32_t& padding) {
            return static_cast<std::int32_t>(render_.client->GetCurrentPadding(&padding));
        });
    if (rf.hr < 0) return !fatal(rf.hr);

    const UINT32 framesToWrite = rf.frames;
    if (framesToWrite == 0) return true;

    BYTE* out = nullptr;
    const HRESULT hrGet = renderService_->GetBuffer(framesToWrite, &out);
    if (FAILED(hrGet)) {
        if (fatal(hrGet)) return false;
        xruns_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const int         ch   = config_.numChannels;
    const std::size_t need = static_cast<std::size_t>(framesToWrite) * idx(ch);

    if (captureRing_.popOrZero(engineIn_.data(), need))
        captureUnderruns_.fetch_add(1, std::memory_order_relaxed);

    callback_->audioDeviceProcess(engineIn_.data(), engineOut_.data(),
                                  static_cast<FrameCount>(framesToWrite));

    remapChannels(engineOut_.data(), ch, deviceScratch_.data(),
                  render_.channels, static_cast<FrameCount>(framesToWrite));
    fromFloat(deviceScratch_.data(), out,
              static_cast<std::size_t>(framesToWrite) * idx(render_.channels),
              render_.sampleFormat);

    renderService_->ReleaseBuffer(framesToWrite, 0);
    return true;
}

void WasapiDevice::threadMain() {
    // Each thread using COM must initialise it. MTA so our objects are usable
    // from here even though they were created on the caller's thread.
    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MmcssScope _;   // "Pro Audio" scheduling for the lifetime of this thread

    // Prefill the render buffer with silence. Starting an empty stream produces
    // an audible click and an immediate underrun on some drivers.
    BYTE* prefill = nullptr;
    if (SUCCEEDED(renderService_->GetBuffer(render_.bufferFrames, &prefill)))
        renderService_->ReleaseBuffer(render_.bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);

    capture_.client->Start();
    render_.client->Start();

    HANDLE waits[3] = { shutdownEvent_, render_.event, capture_.event };

    while (running_.load(std::memory_order_acquire)) {
        const DWORD r = WaitForMultipleObjects(3, waits, FALSE, 2000);
        if (r == WAIT_OBJECT_0) break;          // shutdown
        if (r == WAIT_TIMEOUT) {                // stalled: is the device still there?
            UINT32 padding = 0;
            const HRESULT hr = render_.client->GetCurrentPadding(&padding);
            if (FAILED(hr) && fatal(hr)) break;
            continue;
        }

        if (r == WAIT_OBJECT_0 + 2) { if (!drainCapture()) break; continue; }
        if (r == WAIT_OBJECT_0 + 1) { if (!drainCapture() || !fillRender()) break; continue; }
        break;                                   // WAIT_FAILED
    }

    render_.client->Stop();
    capture_.client->Stop();
    render_.client->Reset();
    capture_.client->Reset();

    if (SUCCEEDED(hrCom)) CoUninitialize();
    running_.store(false, std::memory_order_release);
}

} // namespace rt
#endif
