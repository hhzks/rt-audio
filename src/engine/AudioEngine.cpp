#include "engine/AudioEngine.h"
#include "core/DenormalGuard.h"
#include "core/AtomicPeak.h"
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
    channelPtrs_.clear();
    for (int ch = 0; ch < numChannels; ++ch)
        channelPtrs_.push_back(scratch_.data() + idx(ch) * idx(maxBlockFrames));

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
    stats_.callbackNanos.reset();
    stats_.callbackCount.store(0, std::memory_order_relaxed);
    stats_.xruns.store(0, std::memory_order_relaxed);
    stats_.inputClips.store(0, std::memory_order_relaxed);
    stats_.outputClips.store(0, std::memory_order_relaxed);
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
    std::uint64_t inClips = 0;
    for (int c = 0; c < ch; ++c) {
        float* dst  = channelPtrs_[idx(c)];
        float  peak = 0.0f;
        for (FrameCount i = 0; i < numFrames; ++i) {
            const float s = in[idx(i * ch + c)];
            const float a = std::fabs(s);
            peak = std::max(peak, a);
            if (a >= 1.0f) ++inClips;
            dst[i] = s * inGain;
        }
        raisePeak(stats_.inputPeak[idx(c)], peak);
    }

    // 2. Run the chain in place (unless bypassed).
    AudioBufferView view(channelPtrs_.data(), ch, numFrames);
    if (!params_.bypass.load(std::memory_order_relaxed))
        chain_.process(view);

    // 3. Re-interleave, apply output gain, hard-limit, meter.
    std::uint64_t outClips = 0;
    for (int c = 0; c < ch; ++c) {
        const float* src  = channelPtrs_[idx(c)];
        float        peak = 0.0f;
        for (FrameCount i = 0; i < numFrames; ++i) {
            float v = src[i] * outGain;
            // Safety clamp. A NaN escaping into the driver can wedge the audio
            // engine or produce a very loud noise; neither is acceptable in
            // something you wear headphones for.
            if (!(v > -4.0f && v < 4.0f)) v = 0.0f;
            if (v > 1.0f || v < -1.0f) ++outClips;
            v = std::clamp(v, -1.0f, 1.0f);
            out[idx(i * ch + c)] = v;
            peak = std::max(peak, std::fabs(v));
        }
        raisePeak(stats_.outputPeak[idx(c)], peak);
    }
    if (inClips != 0)  stats_.inputClips.fetch_add(inClips, std::memory_order_relaxed);
    if (outClips != 0) stats_.outputClips.fetch_add(outClips, std::memory_order_relaxed);

    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - t0).count();
    stats_.recordCallback(static_cast<std::uint64_t>(elapsed));
}

void AudioEngine::monitor(const float* in, const float* out, FrameCount numFrames) noexcept {
    if (!prepared_ || numFrames > maxBlockFrames_) {
        stats_.recordXrun();
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const int ch = numChannels_;
    std::uint64_t inClips = 0, outClips = 0;
    for (int c = 0; c < ch; ++c) {
        float inPeak = 0.0f, outPeak = 0.0f;
        for (FrameCount i = 0; i < numFrames; ++i) {
            const float a = std::fabs(in[idx(i * ch + c)]);
            const float b = std::fabs(out[idx(i * ch + c)]);
            inPeak  = std::max(inPeak, a);
            outPeak = std::max(outPeak, b);
            if (a >= 1.0f) ++inClips;
            if (b > 1.0f)  ++outClips;
        }
        raisePeak(stats_.inputPeak[idx(c)], inPeak);
        raisePeak(stats_.outputPeak[idx(c)], outPeak);
    }
    if (inClips != 0)  stats_.inputClips.fetch_add(inClips, std::memory_order_relaxed);
    if (outClips != 0) stats_.outputClips.fetch_add(outClips, std::memory_order_relaxed);

    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - t0).count();
    stats_.recordCallback(static_cast<std::uint64_t>(elapsed));
}

} // namespace rt
