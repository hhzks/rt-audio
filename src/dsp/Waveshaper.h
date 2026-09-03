#pragma once
#include "dsp/IEffect.h"
#include "dsp/Oversampler.h"
#include "core/ParamSmoother.h"
#include "core/ParameterStore.h"
#include <array>
#include <vector>

namespace rt {

// Distortion: drive -> nonlinearity -> makeup -> dry/wet mix, run at 4x so the
// harmonics the shaper generates above Nyquist are filtered off before they can
// fold back down. Costs Oversampler::latencyFrames() of reported latency.
class Waveshaper : public IEffect {
public:
    explicit Waveshaper(const ParameterStore& params) : params_(params) {}

    void prepare(double sampleRate, FrameCount maxBlockFrames, int numChannels) override;
    void reset() override;
    void process(AudioBufferView& io) noexcept override;
    const char* name() const noexcept override { return "Waveshaper"; }

    FrameCount latencyFrames() const noexcept override { return Oversampler::latencyFrames(); }

private:
    static float shape(float x) noexcept;

    const ParameterStore& params_;
    ParamSmoother driveSmoother_;
    ParamSmoother mixSmoother_;

    std::array<Oversampler, kMaxChannels> os_{};
    std::vector<float> up_;   // one channel at 4x, reused across channels
};

} // namespace rt
