#pragma once
#include "core/SpscRingBuffer.h"

#include <cstddef>

namespace rt {

// Push a capture block, evicting the oldest samples when the ring is too full.
// Evictions are rounded up to whole frames so the interleave never splits.
// Returns true if anything was evicted; the caller counts it, because the RT
// thread must not log.
inline bool pushEvictingOldest(SpscRingBuffer& ring, const float* src,
                               std::size_t samples, std::size_t channels) noexcept {
    const std::size_t avail = ring.writeAvailable();
    bool evicted = false;

    if (avail < samples) {
        const std::size_t deficit = samples - avail;
        ring.discard(((deficit + channels - 1) / channels) * channels);
        evicted = true;
    }

    const std::size_t room = ring.writeAvailable();
    const std::size_t fit = (room / channels) * channels;
    const std::size_t clamped_fit = fit < samples ? fit : samples;
    ring.push(src + (samples - clamped_fit), clamped_fit);

    return evicted;
}

// Empty the ring and refill it with `frames` of silence. Call only while
// neither the producer nor the consumer is running.
inline void primeRing(SpscRingBuffer& ring, std::size_t frames, std::size_t channels) noexcept {
    ring.discard(ring.readAvailable());
    ring.pushSilence(frames * channels);
}

} // namespace rt
