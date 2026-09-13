#pragma once
#ifdef RT_HAVE_ALSA

#include "io/IAudioDevice.h"
#include "core/SpscRingBuffer.h"
#include "core/DriftController.h"
#include "dsp/AsyncResampler.h"

#include <alsa/asoundlib.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace rt {

class AlsaDevice : public IAudioDevice {
public:
    ~AlsaDevice() override;

    std::vector<DeviceInfo> enumerate() override;
    void open(const DeviceConfig& config, IAudioCallback* callback) override;
    void start() override;
    void stop() override;
    void close() override;
    DeviceStatus status() const override;
    bool isRunning() const override { return running_.load(std::memory_order_acquire); }

private:
    void openStream(snd_pcm_t*& pcm, const char* name, snd_pcm_stream_t dir,
                    unsigned latencyUs);
    void threadMain();
    void drainCapture(FrameCount frames) noexcept;
    void fillRender(FrameCount frames) noexcept;
    bool handleTransfer(snd_pcm_t* pcm, int err) noexcept;

    snd_pcm_t* capture_ = nullptr;
    snd_pcm_t* render_  = nullptr;
    snd_pcm_uframes_t renderBufferFrames_ = 0;
    std::size_t       ringTargetFrames_ = 0;

    IAudioCallback*   callback_ = nullptr;
    DeviceConfig      config_{};
    DeviceStatus      status_{};
    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> threadReady_{false};
    std::atomic<bool> schedElevated_{false};
    std::atomic<bool> ready_{false};
    std::atomic<std::uint64_t> captureOverruns_{0};
    std::atomic<std::uint64_t> xruns_{0};
    std::atomic<int>           lastErr_{0};

    SpscRingBuffer     captureRing_;
    std::vector<float> engineIn_, engineOut_, captureScratch_, resampleScratch_;

    AsyncResampler  resampler_;
    DriftController drift_;
    double          nominalRatio_ = 1.0;
};

} // namespace rt
#endif
