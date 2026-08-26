#pragma once
#include "io/IAudioDevice.h"
#include <atomic>
#include <thread>
#include <vector>

namespace rt {

// A fake device that drives the callback from a plain thread on a wall-clock
// schedule, feeding a test tone in and discarding the output.
//
// This is not a toy. It means:
//   * the app builds and runs on CI, on Linux, in a container, with no hardware
//   * you can profile the DSP chain's cost in isolation from driver jitter
//   * a new effect can be smoke-tested before you ever plug in an interface
//
// It is NOT low latency and makes no realtime guarantees. That is fine; it is
// not pretending to be a driver.
class NullDevice : public IAudioDevice {
public:
    ~NullDevice() override;

    std::vector<DeviceInfo> enumerate() override;
    void open(const DeviceConfig& config, IAudioCallback* callback) override;
    void start() override;
    void stop() override;
    void close() override;
    DeviceStatus status() const override { return status_; }
    bool isRunning() const override { return running_.load(std::memory_order_acquire); }

private:
    void threadMain();

    IAudioCallback*   callback_ = nullptr;
    DeviceConfig      config_{};
    DeviceStatus      status_{};
    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::vector<float> inBuffer_, outBuffer_;
    double            tonePhase_ = 0.0;
};

} // namespace rt
