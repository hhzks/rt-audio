#pragma once
#include <array>
#include <atomic>
#include <cstdint>

#include "core/Types.h"
#include "engine/RtHistogram.h"

namespace rt {

// Written by the audio thread, read by anyone. Relaxed ordering throughout:
// these are diagnostics, a slightly stale read costs nothing.
//
// The number that matters is `load` = callbackNanos / blockDeadlineNanos.
// Below ~0.5 you have headroom. Above ~0.8 you are one background process away
// from dropouts, regardless of what your average looks like.
struct RtStats {
    std::atomic<std::uint64_t> callbackCount{0};
    std::atomic<std::uint64_t> xruns{0};          // block over maxBlockFrames, or not prepared
    std::atomic<std::uint64_t> lastCallbackNanos{0};
    std::atomic<std::uint64_t> peakCallbackNanos{0};
    std::atomic<std::uint64_t> blockDeadlineNanos{0};
    std::array<std::atomic<float>, kMaxChannels> inputPeak{};    // max |x| since last take
    std::array<std::atomic<float>, kMaxChannels> outputPeak{};
    std::atomic<std::uint64_t> inputClips{0};                    // samples with |x| >= 1
    std::atomic<std::uint64_t> outputClips{0};                   // samples the ±1 clamp changed
    RtHistogram                callbackNanos;

    void recordCallback(std::uint64_t nanos) noexcept {
        callbackCount.fetch_add(1, std::memory_order_relaxed);
        lastCallbackNanos.store(nanos, std::memory_order_relaxed);
        callbackNanos.record(nanos);
        peakCallbackNanos.fetch_max(nanos, std::memory_order_relaxed);
    }

    void recordXrun() noexcept { xruns.fetch_add(1, std::memory_order_relaxed); }

    double loadFactor() const noexcept {
        const auto d = blockDeadlineNanos.load(std::memory_order_relaxed);
        if (d == 0) return 0.0;
        return static_cast<double>(peakCallbackNanos.load(std::memory_order_relaxed)) /
               static_cast<double>(d);
    }

    double loadFactorAt(double p) const noexcept {
        const auto d = blockDeadlineNanos.load(std::memory_order_relaxed);
        if (d == 0) return 0.0;
        return static_cast<double>(callbackNanos.peek().nsAtPercentile(p)) /
               static_cast<double>(d);
    }

    void resetPeaks() noexcept {
        peakCallbackNanos.store(0, std::memory_order_relaxed);
        for (auto& p : inputPeak)  p.store(0.0f, std::memory_order_relaxed);
        for (auto& p : outputPeak) p.store(0.0f, std::memory_order_relaxed);
    }
};

} // namespace rt
