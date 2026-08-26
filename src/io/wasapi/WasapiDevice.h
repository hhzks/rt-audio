#pragma once
#ifdef _WIN32

#include "io/IAudioDevice.h"
#include "io/wasapi/ComPtr.h"
#include "core/SpscRingBuffer.h"

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
    DeviceStatus status() const override {
        DeviceStatus s = status_;
        s.captureOverruns = captureOverruns_.load(std::memory_order_relaxed);
        return s;
    }
    bool isRunning() const override { return running_.load(std::memory_order_acquire); }

private:
    struct Endpoint {
        ComPtr<IMMDevice>    device;
        ComPtr<IAudioClient> client;
        HANDLE               event = nullptr;
        WAVEFORMATEX*        format = nullptr;   // CoTaskMemFree on release
        UINT32               bufferFrames = 0;
        int                  channels = 0;
        void release();
    };

    void initEndpoint(Endpoint& ep, EDataFlow flow, const std::string& id,
                      FrameCount requestedFrames, bool exclusive);
    void threadMain();
    void drainCapture() noexcept;
    void fillRender() noexcept;

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
    HANDLE            shutdownEvent_ = nullptr;
    bool              comInitialized_ = false;

    SpscRingBuffer     captureRing_;   // capture thread -> render, in engine channel layout
    std::vector<float> engineIn_, engineOut_, convertScratch_;
};

} // namespace rt
#endif
