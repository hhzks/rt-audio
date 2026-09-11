#pragma once
#include "dsp/IEffect.h"
#include "core/Types.h"
#include <array>
#include <cstddef>

namespace rt {

// RBJ cookbook biquad, transposed direct form II (good numerical behaviour,
// only two state words per channel).
class Biquad : public IEffect {
public:
    enum class Type { LowPass, HighPass, Peak, LowShelf, HighShelf };
    enum : std::size_t { kFreq = 0, kQ = 1, kGain = 2 };

    Biquad(Type type, double freqHz, double q, double gainDb = 0.0);

    void prepare(double sampleRate, FrameCount maxBlockFrames, int numChannels) override;
    void reset() override;
    void process(AudioBufferView& io) noexcept override;
    const char* name() const noexcept override;

    std::span<const ParamInfo> params() const noexcept override;
    double paramDefault(std::size_t i) const noexcept override { return params_.defaultValue(i); }
    double getParam(std::size_t i) const noexcept override     { return params_.get(i); }
    bool   setParam(std::size_t i, double v) noexcept override;

private:
    void updateCoeffs() noexcept;

    Type          type_;
    ParamBlock<3> params_;
    double        freq_, q_, gainDb_;   // what the current coefficients were computed from
    double        sampleRate_ = 48000.0;

    // b0 b1 b2 a1 a2, already normalised by a0
    double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;

    struct State { double z1 = 0.0, z2 = 0.0; };
    std::array<State, kMaxChannels> state_{};
    int numChannels_ = 0;
};

} // namespace rt
