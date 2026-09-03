#pragma once
#include "core/Types.h"
#include <array>

namespace rt {

// 4x polyphase FIR resampler. One instance per channel; holds no parameters
// and knows nothing about what runs between up and down.
class Oversampler {
public:
    static constexpr int kRatio     = 4;
    static constexpr int kTaps      = 65;
    static constexpr int kPhaseTaps = (kTaps + kRatio - 1) / kRatio;

    static constexpr FrameCount latencyFrames() noexcept { return (kTaps - 1) / kRatio; }

    void prepare();
    void reset() noexcept;

    void upsample(const float* in, float* out, FrameCount n) noexcept;    // n     -> n*4
    void downsample(const float* in, float* out, FrameCount n) noexcept;  // n*4   -> n

private:
    void designKernel() noexcept;

    void pushUp(float s) noexcept {
        upPos_ = (upPos_ + 1 == kPhaseTaps) ? 0 : upPos_ + 1;
        upHist_[idx(upPos_)] = s;
        upHist_[idx(upPos_ + kPhaseTaps)] = s;
    }

    void pushDown(float s) noexcept {
        downPos_ = (downPos_ + 1 == kTaps) ? 0 : downPos_ + 1;
        downHist_[idx(downPos_)] = s;
        downHist_[idx(downPos_ + kTaps)] = s;
    }

    std::array<float, kTaps> kernel_{};                                  // unity DC gain
    std::array<std::array<float, kPhaseTaps>, kRatio> phases_{};         // kernel_ * kRatio

    // Each history is stored twice so that a walk backwards from the newest
    // sample is always contiguous.
    std::array<float, 2 * kPhaseTaps> upHist_{};
    std::array<float, 2 * kTaps>      downHist_{};
    int upPos_   = kPhaseTaps - 1;
    int downPos_ = kTaps - 1;
};

} // namespace rt
