#pragma once
#include "core/DriftController.h"
#include "core/SpscRingBuffer.h"
#include "core/Types.h"
#include "dsp/AsyncResampler.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace rt {

// Producer: beginSource() and push() on one thread. Consumer: pull() and takePeak() on the audio
// thread. prepare() only while neither side runs.
class SystemAudioFeed {
public:
    static constexpr std::size_t kRingFrames = 8192;

    void prepare(int channels, double engineRate, FrameCount maxPullFrames);
    void beginSource(double sourceRate, FrameCount packetFrames);
    void push(const float* in, FrameCount frames) noexcept;
    void pull(float* out, FrameCount n, float gain) noexcept;

    float takePeak() noexcept { return peak_.exchange(0.0f, std::memory_order_relaxed); }
    bool  refilling() const noexcept { return refill_.load(std::memory_order_acquire); }
    std::uint64_t gaps() const noexcept { return gaps_.load(std::memory_order_relaxed); }
    std::uint64_t evictions() const noexcept { return evictions_.load(std::memory_order_relaxed); }
    double trim() const noexcept { return drift_.trim(); }

private:
    static constexpr double kMaxTrim = 0.002;

    SpscRingBuffer     ring_;
    AsyncResampler     resampler_;
    DriftController    drift_;
    std::vector<float> pullScratch_, resampleScratch_;
    int        channels_     = 1;
    double     engineRate_   = 48000.0;
    double     nominal_      = 1.0;
    FrameCount packetFrames_ = 0;
    FrameCount maxPull_      = 0;
    float      lastGain_     = 0.0f;

    std::atomic<std::size_t>   target_{0};
    std::atomic<bool>          refill_{true};
    std::atomic<float>         peak_{0.0f};
    std::atomic<std::uint64_t> gaps_{0};
    std::atomic<std::uint64_t> evictions_{0};
};

} // namespace rt
