#pragma once
#ifdef _WIN32
#include "core/SampleConvert.h"
#include "engine/SystemAudioFeed.h"
#include "io/ISystemAudioTap.h"
#include "io/wasapi/ComPtr.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rt {

class WasapiLoopbackTap final : public ISystemAudioTap {
public:
    WasapiLoopbackTap();
    ~WasapiLoopbackTap() override;
    WasapiLoopbackTap(const WasapiLoopbackTap&) = delete;
    WasapiLoopbackTap& operator=(const WasapiLoopbackTap&) = delete;

    std::vector<SystemSource> sources() override;
    void start(const TapInputs& inputs, double engineRate, int engineChannels,
               FrameCount maxBlock) override;
    void stop() noexcept override;
    void pull(float* out, FrameCount n, float gain) noexcept override { feed_.pull(out, n, gain); }
    float takePeak() noexcept override { return feed_.takePeak(); }
    SystemAudioStatus status() const override;

private:
    class Notifier;

    void threadMain();
    bool openSource();
    void closeSource() noexcept;
    bool drain() noexcept;
    bool fail(HRESULT hr) noexcept;
    std::string resolve(const std::string& id);
    void setState(SystemAudioState state, const std::string& text) noexcept;

    ComPtr<IMMDeviceEnumerator> enumerator_;
    Notifier* notifier_       = nullptr;
    bool      comInitialized_ = false;
    HANDLE    shutdown_ = nullptr;
    HANDLE    reopen_   = nullptr;
    HANDLE    packet_   = nullptr;

    TapInputs       inputs_{};
    int             channels_ = 2;
    SystemAudioFeed feed_;
    std::thread     thread_;
    bool            running_ = false;

    ComPtr<IAudioClient>        client_;
    ComPtr<IAudioCaptureClient> capture_;
    SampleFormat       format_         = SampleFormat::Float32;
    int                sourceChannels_ = 0;
    UINT32             maxFrames_      = 0;
    std::vector<float> deviceScratch_, engineScratch_;

    mutable std::mutex         statusMutex_;
    SystemAudioState           state_ = SystemAudioState::Off;
    std::string                text_;
    std::atomic<std::uint64_t> discontinuities_{0};
};

} // namespace rt
#endif
