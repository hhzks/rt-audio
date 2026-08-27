#include "engine/AudioEngine.h"
#include "core/DenormalGuard.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace rt {

void AudioEngine::prepare(double sampleRate, FrameCount maxBlockFrames, int numChannels) {
    if (numChannels > kMaxChannels)
        throw std::invalid_argument("channel count exceeds kMaxChannels");

    sampleRate_     = sampleRate;
    maxBlockFrames_ = maxBlockFrames;
    numChannels_    = numChannels;

    // The ONLY allocation. Everything downstream borrows from here.
    scratch_.assign(idx(maxBlockFrames) * idx(numChannels), 0.0f);
    for (int ch = 0; ch < numChannels; ++ch)
        channelPtrs_[idx(ch)] = scratch_.data() + idx(ch) * idx(maxBlockFrames);

    chain_.prepare(sampleRate, maxBlockFrames, numChannels);
    chain_.reset();

    const double blockSeconds = static_cast<double>(maxBlockFrames) / sampleRate;
    stats_.blockDeadlineNanos.store(static_cast<std::uint64_t>(blockSeconds * 1e9),
                                    std::memory_order_relaxed);
    prepared_ = true;

    warmUp();
}

void AudioEngine::warmUp() {
    // Run a few silent blocks HERE, on the non-realtime thread, so that the
    // first-touch page faults on scratch_ and the instruction-cache misses on
    // the whole chain happen now rather than inside the first driver callback.
    //
    // Skipping this makes the very first callback ~50x more expensive than the
    // steady state -- measured at 4.9 ms vs 22 us on a 5.3 ms deadline, i.e. an
    // instant dropout plus a peak-load reading that looks like a five-alarm
    // fire on every single startup.
    std::vector<float> silence(idx(maxBlockFrames_) * idx(numChannels_), 0.0f);
    std::vector<float> sink(silence.size(), 0.0f);
    for (int i = 0; i < 8; ++i)
        processInterleaved(silence.data(), sink.data(), maxBlockFrames_);

    chain_.reset();
    stats_.resetPeaks();
    stats_.callbackCount.store(0, std::memory_order_relaxed);
    stats_.xruns.store(0, std::memory_order_relaxed);
}

void AudioEngine::processInterleaved(const float* in, float* out, FrameCount numFrames) noexcept {
    // If this ever fires in production you have a driver handing you a bigger
    // block than it promised. Fail safe (silence) rather than corrupt memory.
    if (!prepared_ || numFrames > maxBlockFrames_) {
        std::memset(out, 0, sizeof(float) * idx(numFrames) * idx(numChannels_));
        stats_.recordXrun();
        return;
    }

    DenormalGuard _;
    const auto t0 = std::chrono::steady_clock::now();

    const int ch = numChannels_;
    const float inGain  = params_.inputGain.load(std::memory_order_relaxed);
    const float outGain = params_.outputGain.load(std::memory_order_relaxed);

    // 1. De-interleave into planar scratch, applying input gain.
    for (int c = 0; c < ch; ++c) {
        float* dst = channelPtrs_[idx(c)];
        for (FrameCount i = 0; i < numFrames; ++i)
            dst[i] = in[idx(i * ch + c)] * inGain;
    }

    // 2. Run the chain in place (unless bypassed).
    AudioBufferView view(channelPtrs_.data(), ch, numFrames);
    if (!params_.bypass.load(std::memory_order_relaxed))
        chain_.process(view);

    // 3. Re-interleave, apply output gain, hard-limit, track peak.
    float peak = 0.0f;
    for (int c = 0; c < ch; ++c) {
        const float* src = channelPtrs_[idx(c)];
        for (FrameCount i = 0; i < numFrames; ++i) {
            float v = src[i] * outGain;
            // Safety clamp. A NaN escaping into the driver can wedge the audio
            // engine or produce a very loud noise; neither is acceptable in
            // something you wear headphones for.
            if (!(v > -4.0f && v < 4.0f)) v = 0.0f;
            v = std::clamp(v, -1.0f, 1.0f);
            out[idx(i * ch + c)] = v;
            peak = std::max(peak, std::fabs(v));
        }
    }

    auto prevPeak = stats_.peakOutputLevel.load(std::memory_order_relaxed);
    if (peak > prevPeak) stats_.peakOutputLevel.store(peak, std::memory_order_relaxed);

    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - t0).count();
    stats_.recordCallback(static_cast<std::uint64_t>(elapsed));
}

} // namespace rt
