#pragma once
#include "core/Types.h"
#include <vector>

namespace rt {

// Arbitrary-ratio resampler for a duplex stream whose capture and render ends
// are not the same crystal. `ratio` is input frames consumed per output frame
// and is expected to move while running: the nominal rate difference is the
// coarse part of it, drift correction the fine part.
//
// One object handles every channel, interleaved. The phase accumulator is
// shared -- the channels are synchronous by definition -- and only the FIR
// history is per channel, so the phase table is not duplicated into cache once
// per channel.
class AsyncResampler {
public:
    static constexpr int kPhases = 64;
    static constexpr int kTaps   = 32;

    // Group delay, in INPUT frames.
    static constexpr FrameCount latencyFrames() noexcept { return kTaps / 2; }

    // Allocates. `nominalRatio` fixes the anti-alias cutoff; later setRatio()
    // trims for drift without redesigning the table.
    void prepare(int channels, double nominalRatio);
    void reset() noexcept;

    void   setRatio(double ratio) noexcept { ratio_ = ratio; }
    double ratio() const noexcept { return ratio_; }

    // Consumes up to `inFrames`, writes however many output frames fall out and
    // returns that count. Allocation-free.
    //
    // Undersizing `maxOutFrames` silently drops the tail of the input rather
    // than overrunning `out`; size it with maxOutputFor().
    FrameCount process(const float* in, FrameCount inFrames,
                       float* out, FrameCount maxOutFrames) noexcept;

    // Upper bound on what `inFrames` can produce at `ratio`. For scratch sizing.
    static FrameCount maxOutputFor(FrameCount inFrames, double ratio) noexcept;

private:
    void designTable(double ratio);
    void pushFrame(const float* frame) noexcept;

    std::vector<float> table_;   // (kPhases + 1) rows of kTaps, row-major
    std::vector<float> hist_;    // channels * 2 * kTaps; each sample stored twice
    int    channels_ = 0;
    int    pos_      = kTaps - 1;
    double phase_    = 1.0;      // >= 1.0 means "consume before emitting"
    double ratio_    = 1.0;
};

} // namespace rt
