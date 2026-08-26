#pragma once
#include "core/AudioBufferView.h"

namespace rt {

// The contract every effect obeys.
//
//   prepare() -- called on a NON-realtime thread. Allocate everything here.
//   reset()   -- clear state (filter memory, delay lines). Also non-RT.
//   process() -- called on the RT thread. MUST NOT: allocate, lock, throw,
//                log, touch a file, or call anything that might block.
//
// The `noexcept` on process() is load-bearing: it makes an accidental throw a
// std::terminate instead of unwinding through the driver callback.
class IEffect {
public:
    virtual ~IEffect() = default;

    virtual void prepare(double sampleRate, FrameCount maxBlockFrames, int numChannels) = 0;
    virtual void reset() = 0;
    virtual void process(AudioBufferView& io) noexcept = 0;

    virtual const char* name() const noexcept = 0;

    // Extra latency this effect introduces, in frames. An oversampler's FIR or
    // an STFT denoiser's analysis window both report here so the host can
    // compensate. Report honestly or your delay compensation will be wrong.
    virtual FrameCount latencyFrames() const noexcept { return 0; }
};

} // namespace rt
