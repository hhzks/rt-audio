#pragma once
#include "dsp/IEffect.h"
#include "core/Types.h"
#include <array>

namespace rt {

// RBJ cookbook biquad, transposed direct form II (good numerical behaviour,
// only two state words per channel).
class Biquad : public IEffect {
public:
    enum class Type { LowPass, HighPass, Peak, LowShelf, HighShelf };

    Biquad(Type type, double freqHz, double q, double gainDb = 0.0)
        : type_(type), freq_(freqHz), q_(q), gainDb_(gainDb) {}

    void prepare(double sampleRate, FrameCount maxBlockFrames, int numChannels) override;
    void reset() override;
    void process(AudioBufferView& io) noexcept override;
    const char* name() const noexcept override { return "Biquad"; }

    // Non-RT: recompute coefficients. To change these live, post a command and
    // apply it at block boundaries rather than calling this from the UI thread.
    void setParams(double freqHz, double q, double gainDb = 0.0);

private:
    void updateCoeffs();

    Type   type_;
    double freq_, q_, gainDb_;
    double sampleRate_ = 48000.0;

    // b0 b1 b2 a1 a2, already normalised by a0
    double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;

    struct State { double z1 = 0.0, z2 = 0.0; };
    std::array<State, kMaxChannels> state_{};
    int numChannels_ = 0;
};

} // namespace rt
