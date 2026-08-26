#pragma once
#include "dsp/IEffect.h"
#include "core/ParamSmoother.h"
#include "core/ParameterStore.h"
#include <array>

namespace rt {

// Distortion: drive -> nonlinearity -> makeup -> dry/wet mix.
//
// IMPORTANT LIMITATION, READ BEFORE SHIPPING:
// this shapes at the base rate, so it aliases. Any nonlinearity generates
// harmonics above Nyquist which fold back down as inharmonic garbage --
// audible as a metallic buzz on high notes. Fix is 4-8x oversampling around
// the shaper only (see Oversampler in the next-steps list). Left out here so
// the first version stays readable.
class Waveshaper : public IEffect {
public:
    explicit Waveshaper(const ParameterStore& params) : params_(params) {}

    void prepare(double sampleRate, FrameCount maxBlockFrames, int numChannels) override;
    void reset() override;
    void process(AudioBufferView& io) noexcept override;
    const char* name() const noexcept override { return "Waveshaper"; }

private:
    static float shape(float x) noexcept;

    const ParameterStore& params_;
    ParamSmoother driveSmoother_;
    ParamSmoother mixSmoother_;
};

} // namespace rt
