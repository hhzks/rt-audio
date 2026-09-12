#pragma once
#include "core/AudioBufferView.h"
#include "core/ParameterStore.h"
#include "dsp/EffectChain.h"
#include "engine/RtStats.h"
#include <array>
#include <vector>

namespace rt {

// The bridge between interleaved device buffers and the planar world the DSP
// wants. Knows nothing about WASAPI, ASIO, ALSA, or files -- which is exactly
// why the offline_render tool can drive it with no audio hardware at all.
class AudioEngine {
public:
    void prepare(double sampleRate, FrameCount maxBlockFrames, int numChannels);

    // Called automatically by prepare(). Pre-faults buffers and warms caches on
    // the calling (non-RT) thread. Call again after changing the chain.
    void warmUp();

    // THE REALTIME ENTRY POINT. Everything it touches is preallocated.
    // `in` and `out` are interleaved, numChannels * numFrames floats.
    // Aliasing in == out is allowed.
    void processInterleaved(const float* in, float* out, FrameCount numFrames) noexcept;

    // Realtime. Records the callback and the meters from `in` and `out` without
    // running the chain or writing `out`.
    void monitor(const float* in, const float* out, FrameCount numFrames) noexcept;

    ParameterStore& params()       noexcept { return params_; }
    RtStats&        stats()        noexcept { return stats_; }
    EffectChain&    chain()        noexcept { return chain_; }
    const EffectChain& chain() const noexcept { return chain_; }

    double     sampleRate()   const noexcept { return sampleRate_; }
    int        numChannels()  const noexcept { return numChannels_; }
    FrameCount maxBlockFrames() const noexcept { return maxBlockFrames_; }

private:
    ParameterStore params_;
    RtStats        stats_;
    EffectChain    chain_;

    double     sampleRate_     = 48000.0;
    FrameCount maxBlockFrames_ = 0;
    int        numChannels_    = 0;
    bool       prepared_       = false;

    std::vector<float>                     scratch_;      // planar storage
    std::array<float*, kMaxChannels>       channelPtrs_{}; // views into scratch_
};

} // namespace rt
