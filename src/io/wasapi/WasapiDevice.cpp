#ifdef _WIN32
#include "io/wasapi/WasapiDevice.h"
#include "io/wasapi/MmcssScope.h"

#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace rt {
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

// We only handle 32-bit float here. Shared mode virtually always gives you
// float from GetMixFormat, so this is not as restrictive as it sounds -- but
// EXCLUSIVE mode will hand you 16 or 24-bit integer and you must convert.
bool isFloat32(const WAVEFORMATEX* fmt) {
    if (fmt->wBitsPerSample != 32) return false;
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        return ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

// Map `srcCh` interleaved channels to `dstCh`. Mono in -> duplicate to all;
// more in than out -> take the first N; fewer -> repeat the last.
void remapChannels(const float* src, int srcCh, float* dst, int dstCh, FrameCount frames) noexcept {
    if (srcCh == dstCh) {
        std::memcpy(dst, src, sizeof(float) * static_cast<std::size_t>(frames) * idx(dstCh));
        return;
    }
    for (FrameCount i = 0; i < frames; ++i) {
        const float* s = src + static_cast<std::size_t>(i) * idx(srcCh);
        float*       d = dst + static_cast<std::size_t>(i) * idx(dstCh);
        for (int c = 0; c < dstCh; ++c) d[c] = s[std::min(c, srcCh - 1)];
    }
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
    if (!isFloat32(ep.format))
        throw std::runtime_error(
            "endpoint mix format is not 32-bit float. Add a sample-format converter "
            "in initEndpoint before proceeding (see TODO in WasapiDevice.cpp).");

    ep.channels = ep.format->nChannels;

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

    if (capture_.format->nSamplesPerSec != render_.format->nSamplesPerSec)
        throw std::runtime_error(
            "capture and render are running at different sample rates. Set both to the "
            "same rate in Windows Sound settings, or add a resampler in drainCapture().");

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

    // Ring holds ~4 render buffers' worth. Too small and normal jitter causes
    // dropouts; too large and you have added pure latency for nothing.
    captureRing_.reset(maxBlock * idx(engineCh) * 4);

    engineIn_.assign(maxBlock * idx(engineCh), 0.0f);
    engineOut_.assign(maxBlock * idx(engineCh), 0.0f);
    convertScratch_.assign(maxBlock * idx(std::max(capture_.channels, render_.channels)), 0.0f);

    status_.sampleRate  = sr;
    status_.blockFrames = static_cast<FrameCount>(render_.bufferFrames);
    status_.numChannels = engineCh;
    status_.backendName = config.exclusiveMode ? "WASAPI (exclusive)" : "WASAPI (shared)";
    status_.estimatedRoundTripMs =
        1000.0 * (capture_.bufferFrames + render_.bufferFrames) / sr;

    shutdownEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

// --------------------------------------------------------- transport + RT --

void WasapiDevice::start() {
    if (running_.exchange(true)) return;
    ResetEvent(shutdownEvent_);
    thread_ = std::thread(&WasapiDevice::threadMain, this);
}

void WasapiDevice::stop() {
    if (!running_.exchange(false)) return;
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

void WasapiDevice::drainCapture() noexcept {
    UINT32 packetFrames = 0;
    while (SUCCEEDED(captureService_->GetNextPacketSize(&packetFrames)) && packetFrames > 0) {
        BYTE*  data  = nullptr;
        UINT32 frames = 0;
        DWORD  flags  = 0;
        if (FAILED(captureService_->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
            break;

        const auto* src = reinterpret_cast<const float*>(data);
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            std::memset(convertScratch_.data(), 0,
                        sizeof(float) * static_cast<std::size_t>(frames) * idx(config_.numChannels));
        } else {
            remapChannels(src, capture_.channels, convertScratch_.data(),
                          config_.numChannels, static_cast<FrameCount>(frames));
        }

        const std::size_t ch   = static_cast<std::size_t>(config_.numChannels);
        const std::size_t want = static_cast<std::size_t>(frames) * ch;

        // Evict oldest, rounded to whole frames, so the interleave never splits.
        const std::size_t avail = captureRing_.writeAvailable();
        if (avail < want) {
            captureOverruns_.fetch_add(1, std::memory_order_relaxed);
            const std::size_t deficit = want - avail;
            captureRing_.discard(((deficit + ch - 1) / ch) * ch);
        }
        captureRing_.push(convertScratch_.data(), want);
        captureService_->ReleaseBuffer(frames);
    }
}

void WasapiDevice::fillRender() noexcept {
    UINT32 padding = 0;
    if (FAILED(render_.client->GetCurrentPadding(&padding))) return;

    const UINT32 framesToWrite = render_.bufferFrames - padding;
    if (framesToWrite == 0) return;

    BYTE* out = nullptr;
    if (FAILED(renderService_->GetBuffer(framesToWrite, &out))) return;

    const int         ch   = config_.numChannels;
    const std::size_t need = static_cast<std::size_t>(framesToWrite) * idx(ch);

    captureRing_.popOrZero(engineIn_.data(), need);

    callback_->audioDeviceProcess(engineIn_.data(), engineOut_.data(),
                                  static_cast<FrameCount>(framesToWrite));

    remapChannels(engineOut_.data(), ch, reinterpret_cast<float*>(out),
                  render_.channels, static_cast<FrameCount>(framesToWrite));

    renderService_->ReleaseBuffer(framesToWrite, 0);
}

void WasapiDevice::threadMain() {
    // Each thread using COM must initialise it. MTA so our objects are usable
    // from here even though they were created on the caller's thread.
    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MmcssScope mmcss;   // "Pro Audio" scheduling for the lifetime of this thread

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
        if (r == WAIT_TIMEOUT)  continue;       // device stalled; loop and retry

        if (r == WAIT_OBJECT_0 + 2) { drainCapture(); continue; }
        if (r == WAIT_OBJECT_0 + 1) { drainCapture(); fillRender(); continue; }
        break;                                   // WAIT_FAILED
    }

    render_.client->Stop();
    capture_.client->Stop();
    render_.client->Reset();
    capture_.client->Reset();

    if (SUCCEEDED(hrCom)) CoUninitialize();
}

} // namespace rt
#endif
