#pragma once
#ifdef _WIN32

#include "io/IAudioDevice.h"
#include "io/wasapi/ComPtr.h"
#include "io/wasapi/WasapiError.h"
#include "io/wasapi/WasapiRender.h"
#include "core/SampleConvert.h"
#include "core/SpscRingBuffer.h"
#include "core/DriftController.h"
#include "dsp/AsyncResampler.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace rt {

// Event-driven duplex WASAPI, shared mode, via IAudioClient3 where available.
//
// The hard problem this solves is that capture and render are two separate
// devices on two separate crystals. They drift. The SpscRingBuffer between
// them absorbs that; see DriftCompensator in the next-steps list for holding
// the fill level steady over hours rather than minutes.
class WasapiDevice : public IAudioDevice {
public:
    WasapiDevice();
    ~WasapiDevice() override;

    std::vector<DeviceInfo> enumerate() override;
    void open(const DeviceConfig& config, IAudioCallback* callback) override;
    void start() override;
    void stop() override;
    void close() override;
    DeviceStatus status() const override;
    bool isRunning() const override { return running_.load(std::memory_order_acquire); }

private:
    struct Endpoint {
        ComPtr<IMMDevice>    device;
        ComPtr<IAudioClient> client;
        HANDLE               event = nullptr;
        WAVEFORMATEX*        format = nullptr;   // CoTaskMemFree on release
        SampleFormat         sampleFormat = SampleFormat::Float32;
        UINT32               bufferFrames = 0;
        int                  channels = 0;
        void release();
    };

    void initEndpoint(Endpoint& ep, EDataFlow flow, const std::string& id,
                      FrameCount requestedFrames, bool exclusive);
    void threadMain();
    bool drainCapture() noexcept;   // false: fatal error, stop the thread
    bool fillRender() noexcept;
    bool fatal(HRESULT hr) noexcept;

    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IAudioCaptureClient> captureService_;
    ComPtr<IAudioRenderClient>  renderService_;

    Endpoint capture_, render_;

    IAudioCallback*   callback_ = nullptr;
    DeviceConfig      config_{};
    DeviceStatus      status_{};
    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> captureOverruns_{0};
    std::atomic<std::uint64_t> captureUnderruns_{0};
    std::atomic<std::uint64_t> xruns_{0};
    std::atomic<long>          lastHr_{0};
    HANDLE            shutdownEvent_ = nullptr;
    bool              comInitialized_ = false;

    SpscRingBuffer     captureRing_;   // capture thread -> render, in RENDER-rate engine layout
    std::size_t        ringTargetFrames_ = 0;
    std::vector<float> engineIn_, engineOut_, convertScratch_, deviceScratch_, resampleScratch_;

    AsyncResampler  resampler_;
    DriftController drift_;
    double          nominalRatio_ = 1.0;   // capture frames per render frame
};

} // namespace rt
#endif
