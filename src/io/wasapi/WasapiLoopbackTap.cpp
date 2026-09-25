#ifdef _WIN32
#include "io/wasapi/WasapiLoopbackTap.h"
#include "core/ChannelMap.h"
#include "io/TapPlan.h"
#include "io/WinString.h"
#include "io/wasapi/MmcssScope.h"
#include "io/wasapi/WasapiEndpoint.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace rt {

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
class WasapiLoopbackTap::Notifier final : public IMMNotificationClient {
public:
    explicit Notifier(HANDLE reopen) : reopen_(reopen) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) SetEvent(reopen_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    HANDLE             reopen_;
};
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

WasapiLoopbackTap::WasapiLoopbackTap() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    comInitialized_ = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) throw std::runtime_error("CoInitializeEx failed");
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), enumerator_.putVoid()))) {
        if (comInitialized_) CoUninitialize();
        throw std::runtime_error("CoCreateInstance(MMDeviceEnumerator) failed");
    }
    shutdown_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    reopen_   = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    packet_   = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    notifier_ = new Notifier(reopen_);
    enumerator_->RegisterEndpointNotificationCallback(notifier_);
}

WasapiLoopbackTap::~WasapiLoopbackTap() {
    stop();
    enumerator_->UnregisterEndpointNotificationCallback(notifier_);
    notifier_->Release();
    CloseHandle(packet_);
    CloseHandle(reopen_);
    CloseHandle(shutdown_);
    enumerator_.reset();
    if (comInitialized_) CoUninitialize();
}

std::vector<SystemSource> WasapiLoopbackTap::sources() {
    std::vector<SystemSource> out;
    ComPtr<IMMDeviceCollection> list;
    if (FAILED(enumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, list.put()))) return out;
    UINT n = 0;
    list->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(list->Item(i, dev.put()))) continue;
        out.push_back({endpointId(dev.get()), friendlyName(dev.get())});
    }
    return out;
}

void WasapiLoopbackTap::start(const TapInputs& inputs, double engineRate, int engineChannels,
                              FrameCount maxBlock) {
    stop();
    inputs_   = inputs;
    channels_ = engineChannels;
    feed_.prepare(engineChannels, engineRate, maxBlock);
    discontinuities_.store(0, std::memory_order_relaxed);
    setState(SystemAudioState::Idle, "");
    ResetEvent(shutdown_);
    ResetEvent(reopen_);
    running_ = true;
    thread_  = std::thread(&WasapiLoopbackTap::threadMain, this);
}

void WasapiLoopbackTap::stop() noexcept {
    if (!running_) return;
    SetEvent(shutdown_);
    if (thread_.joinable()) thread_.join();
    running_ = false;
    setState(SystemAudioState::Off, "");
}

SystemAudioStatus WasapiLoopbackTap::status() const {
    SystemAudioStatus s;
    {
        std::lock_guard lock(statusMutex_);
        s.state = state_;
        s.text  = text_;
    }
    if (s.state == SystemAudioState::Idle || s.state == SystemAudioState::Playing)
        s.state = feed_.refilling() ? SystemAudioState::Idle : SystemAudioState::Playing;
    s.gaps            = feed_.gaps();
    s.evictions       = feed_.evictions();
    s.discontinuities = discontinuities_.load(std::memory_order_relaxed);
    return s;
}

void WasapiLoopbackTap::setState(SystemAudioState state, const std::string& text) noexcept {
    try {
        std::lock_guard lock(statusMutex_);
        state_ = state;
        text_  = text;
    } catch (...) {
    }
}

std::string WasapiLoopbackTap::resolve(const std::string& id) {
    if (!id.empty()) return id;
    ComPtr<IMMDevice> dev;
    if (FAILED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, dev.put()))) return {};
    return endpointId(dev.get());
}

bool WasapiLoopbackTap::openSource() {
    const std::string source = resolve(inputs_.sourceId);
    if (source.empty()) {
        setState(SystemAudioState::Error, "no output device in Windows");
        return false;
    }
    const std::string output =
        inputs_.backend == Backend::Wasapi ? resolve(inputs_.outputId) : std::string();

    ComPtr<IMMDevice> dev;
    if (FAILED(enumerator_->GetDevice(utf8ToWide(source).c_str(), dev.put()))) {
        setState(SystemAudioState::Error, "source removed");
        return false;
    }
    const std::string name = friendlyName(dev.get());
    if (tapPlan(inputs_, source, output) == TapAction::SameDevice) {
        setState(SystemAudioState::SameDevice,
                 name + " is the rt-audio output; set a different default output in Windows");
        return false;
    }

    WAVEFORMATEX* mix = nullptr;
    REFERENCE_TIME period = 0;
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.putVoid());
    if (SUCCEEDED(hr)) hr = client_->GetMixFormat(&mix);
    if (FAILED(hr)) return fail(hr);

    const auto fmt = detectFormat(mix);
    if (!fmt) {
        CoTaskMemFree(mix);
        closeSource();
        setState(SystemAudioState::Error, name + ": unsupported mix format");
        return false;
    }
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                             0, 0, mix, nullptr);
    const double rate  = mix->nSamplesPerSec;
    sourceChannels_    = mix->nChannels;
    format_            = *fmt;
    CoTaskMemFree(mix);
    if (SUCCEEDED(hr)) hr = client_->SetEventHandle(packet_);
    if (SUCCEEDED(hr)) hr = client_->GetBufferSize(&maxFrames_);
    if (SUCCEEDED(hr)) hr = client_->GetDevicePeriod(&period, nullptr);
    if (SUCCEEDED(hr)) hr = client_->GetService(__uuidof(IAudioCaptureClient), capture_.putVoid());
    if (FAILED(hr)) return fail(hr);

    deviceScratch_.assign(idx(static_cast<int>(maxFrames_)) * idx(sourceChannels_), 0.0f);
    engineScratch_.assign(idx(static_cast<int>(maxFrames_)) * idx(channels_), 0.0f);
    const auto packetFrames = static_cast<FrameCount>(
        static_cast<double>(period) * rate / 1.0e7 + 0.5);
    feed_.beginSource(rate, std::max<FrameCount>(packetFrames, 1));

    hr = client_->Start();
    if (FAILED(hr)) return fail(hr);
    setState(SystemAudioState::Idle, name);
    return true;
}

void WasapiLoopbackTap::closeSource() noexcept {
    if (client_) client_->Stop();
    capture_.reset();
    client_.reset();
}

bool WasapiLoopbackTap::fail(HRESULT hr) noexcept {
    closeSource();
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        setState(SystemAudioState::Error, "source removed");
        return false;
    }
    try {
        std::ostringstream os;
        os << "loopback failed (hr=0x" << std::hex << static_cast<unsigned long>(hr) << ")";
        setState(SystemAudioState::Error, os.str());
    } catch (...) {
    }
    return false;
}

bool WasapiLoopbackTap::drain() noexcept {
    for (;;) {
        UINT32 packet = 0;
        HRESULT hr = capture_->GetNextPacketSize(&packet);
        if (FAILED(hr)) return fail(hr);
        if (packet == 0) return true;

        BYTE*  data   = nullptr;
        UINT32 frames = 0;
        DWORD  flags  = 0;
        hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) return fail(hr);

        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
            discontinuities_.fetch_add(1, std::memory_order_relaxed);

        const auto n = static_cast<FrameCount>(std::min(frames, maxFrames_));
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            std::fill_n(engineScratch_.data(), idx(n) * idx(channels_), 0.0f);
        } else {
            toFloat(data, deviceScratch_.data(), idx(n) * idx(sourceChannels_), format_);
            remapChannels(deviceScratch_.data(), sourceChannels_, engineScratch_.data(),
                          channels_, n);
        }
        feed_.push(engineScratch_.data(), n);
        capture_->ReleaseBuffer(frames);
    }
}

void WasapiLoopbackTap::threadMain() {
    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MmcssScope _;
    bool open = false;
    for (;;) {
        if (!open) open = openSource();
        HANDLE waits[3] = {shutdown_, reopen_, packet_};
        const DWORD r = WaitForMultipleObjects(open ? 3 : 2, waits, FALSE, 1000);
        if (r == WAIT_OBJECT_0) break;
        if (r == WAIT_OBJECT_0 + 1) {
            closeSource();
            open = false;
            continue;
        }
        if (r == WAIT_OBJECT_0 + 2) {
            if (!drain()) open = false;
            continue;
        }
        if (r == WAIT_TIMEOUT) continue;
        break;
    }
    closeSource();
    if (SUCCEEDED(hrCom)) CoUninitialize();
}

} // namespace rt
#endif
