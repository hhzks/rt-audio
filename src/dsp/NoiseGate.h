#pragma once
#include "dsp/IEffect.h"
#include <array>
#include <cstddef>

namespace rt {

// PLACEHOLDER noise suppressor: a broadband downward expander.
//
// This is deliberately the simplest thing that reduces steady background hiss
// between words, so you have a working slot in the chain on day one. It is not
// competitive with a real denoiser -- it cannot remove noise while you are
// speaking, only between phrases, and it will pump on transients.
//
// Replace with one of:
//   * RNNoise      -- 48 kHz, 10 ms frames, tiny, MIT-ish, easy to link
//   * DeepFilterNet -- better quality, heavier
//   * your own STFT Wiener filter (then set latencyFrames() to the window!)
class NoiseGate : public IEffect {
public:
    enum : std::size_t { kOn = 0, kThreshold = 1, kGainReduction = 2 };

    NoiseGate();

    void prepare(double sampleRate, FrameCount maxBlockFrames, int numChannels) override;
    void reset() override;
    void process(AudioBufferView& io) noexcept override;
    const char* name() const noexcept override { return "Gate"; }

    std::span<const ParamInfo> params() const noexcept override { return params_.info(); }
    double paramDefault(std::size_t i) const noexcept override { return params_.defaultValue(i); }
    double getParam(std::size_t i) const noexcept override     { return params_.get(i); }
    bool   setParam(std::size_t i, double v) noexcept override { return params_.set(i, v); }

private:
    ParamBlock<3> params_;
    double sampleRate_ = 48000.0;
    float  attackCoeff_ = 0.0f, releaseCoeff_ = 0.0f, envCoeff_ = 0.0f;
    std::array<float, kMaxChannels> env_{};   // signal envelope, per channel
    std::array<float, kMaxChannels> gain_{};  // smoothed gate gain, per channel
};

} // namespace rt
