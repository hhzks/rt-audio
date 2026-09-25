#include "engine/SystemAudioFeed.h"
#include "core/AtomicPeak.h"
#include "core/RingPush.h"

#include <algorithm>
#include <cmath>

namespace rt {

void SystemAudioFeed::prepare(int channels, double engineRate, FrameCount maxPullFrames) {
    channels_   = std::clamp(channels, 1, kMaxChannels);
    engineRate_ = engineRate;
    maxPull_    = std::max<FrameCount>(maxPullFrames, 1);
    ring_.reset(kRingFrames * idx(channels_));
    pullScratch_.assign(idx(maxPull_) * idx(channels_), 0.0f);
    packetFrames_ = 0;
    lastGain_     = 0.0f;
    target_.store(0, std::memory_order_relaxed);
    refill_.store(true, std::memory_order_relaxed);
    peak_.store(0.0f, std::memory_order_relaxed);
    gaps_.store(0, std::memory_order_relaxed);
    evictions_.store(0, std::memory_order_relaxed);
}

void SystemAudioFeed::beginSource(double sourceRate, FrameCount packetFrames) {
    packetFrames_ = std::max<FrameCount>(packetFrames, 1);
    nominal_      = sourceRate / engineRate_;
    resampler_.prepare(channels_, nominal_);
    resampleScratch_.assign(
        idx(AsyncResampler::maxOutputFor(packetFrames_, nominal_ * (1.0 - kMaxTrim)))
            * idx(channels_), 0.0f);
    const auto packetOut = static_cast<std::size_t>(
        std::lround(static_cast<double>(packetFrames_) / nominal_));
    const std::size_t packetSamples = packetOut * idx(channels_);
    drift_.prepare(packetSamples, kMaxTrim);
    target_.store(2 * packetSamples, std::memory_order_release);
}

void SystemAudioFeed::push(const float* in, FrameCount frames) noexcept {
    if (packetFrames_ <= 0) return;
    const std::size_t ch = idx(channels_);
    for (FrameCount done = 0; done < frames;) {
        const FrameCount len = std::min(frames - done, packetFrames_);
        if (!refill_.load(std::memory_order_acquire))
            resampler_.setRatio(nominal_ * drift_.update(ring_.readAvailable()));
        const FrameCount produced = resampler_.process(
            in + idx(done) * ch, len, resampleScratch_.data(),
            static_cast<FrameCount>(resampleScratch_.size() / ch));
        if (pushEvictingOldest(ring_, resampleScratch_.data(), idx(produced) * ch, ch))
            evictions_.fetch_add(1, std::memory_order_relaxed);
        done += len;
    }
}

void SystemAudioFeed::pull(float* out, FrameCount n, float gain) noexcept {
    if (n <= 0 || n > maxPull_) return;
    const std::size_t ch      = idx(channels_);
    const std::size_t samples = idx(n) * ch;

    if (refill_.load(std::memory_order_relaxed)) {
        const std::size_t target = target_.load(std::memory_order_acquire);
        if (target == 0 || ring_.readAvailable() < target) {
            lastGain_ = gain;
            return;
        }
        refill_.store(false, std::memory_order_release);
    }

    const std::size_t got = ring_.pop(pullScratch_.data(), samples);
    if (got < samples) {
        std::fill(pullScratch_.begin() + static_cast<std::ptrdiff_t>(got),
                  pullScratch_.begin() + static_cast<std::ptrdiff_t>(samples), 0.0f);
        refill_.store(true, std::memory_order_release);
        gaps_.fetch_add(1, std::memory_order_relaxed);
    }

    const float g0   = lastGain_;
    const float step = (gain - g0) / static_cast<float>(n);
    float peak = 0.0f;
    for (FrameCount i = 0; i < n; ++i) {
        const float g = g0 + step * static_cast<float>(i + 1);
        for (std::size_t c = 0; c < ch; ++c) {
            const std::size_t k = idx(i) * ch + c;
            const float s = pullScratch_[k] * g;
            peak   = std::max(peak, std::fabs(s));
            out[k] = std::clamp(out[k] + s, -1.0f, 1.0f);
        }
    }
    lastGain_ = gain;
    raisePeak(peak_, peak);
}

} // namespace rt
