#pragma once
#include <atomic>
#include <cstdint>

namespace rt {

// Written by the audio thread, read by anyone. Relaxed ordering throughout:
// these are diagnostics, a slightly stale read costs nothing.
//
// The number that matters is `load` = callbackNanos / blockDeadlineNanos.
// Below ~0.5 you have headroom. Above ~0.8 you are one background process away
// from dropouts, regardless of what your average looks like.
struct RtStats {
    std::atomic<std::uint64_t> callbackCount{0};
    std::atomic<std::uint64_t> xruns{0};          // ring ran dry / overflowed
    std::atomic<std::uint64_t> lastCallbackNanos{0};
    std::atomic<std::uint64_t> peakCallbackNanos{0};
    std::atomic<std::uint64_t> blockDeadlineNanos{0};
    std::atomic<float>         peakOutputLevel{0.0f};

    void recordCallback(std::uint64_t nanos) noexcept {
        callbackCount.fetch_add(1, std::memory_order_relaxed);
        lastCallbackNanos.store(nanos, std::memory_order_relaxed);
        auto prev = peakCallbackNanos.load(std::memory_order_relaxed);
        while (nanos > prev &&
               !peakCallbackNanos.compare_exchange_weak(prev, nanos, std::memory_order_relaxed)) {}
    }

    void recordXrun() noexcept { xruns.fetch_add(1, std::memory_order_relaxed); }

    double loadFactor() const noexcept {
        const auto d = blockDeadlineNanos.load(std::memory_order_relaxed);
        if (d == 0) return 0.0;
        return static_cast<double>(peakCallbackNanos.load(std::memory_order_relaxed)) /
               static_cast<double>(d);
    }

    void resetPeaks() noexcept {
        peakCallbackNanos.store(0, std::memory_order_relaxed);
        peakOutputLevel.store(0.0f, std::memory_order_relaxed);
    }
};

} // namespace rt
