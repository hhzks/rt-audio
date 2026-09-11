#pragma once
#include "dsp/IEffect.h"
#include "dsp/Oversampler.h"
#include "core/ParamSmoother.h"
#include <array>
#include <cstddef>
#include <vector>

namespace rt {

// Distortion: drive -> nonlinearity -> makeup -> dry/wet mix, run at 4x so the
// harmonics the shaper generates above Nyquist are filtered off before they can
// fold back down. Costs Oversampler::latencyFrames() of reported latency.
class Waveshaper : public IEffect {
public:
    enum : std::size_t { kOn = 0, kDrive = 1, kMix = 2 };

    Waveshaper();

    void prepare(double sampleRate, FrameCount maxBlockFrames, int numChannels) override;
    void reset() override;
    void process(AudioBufferView& io) noexcept override;
    const char* name() const noexcept override { return "Drive"; }

    FrameCount latencyFrames() const noexcept override { return Oversampler::latencyFrames(); }

    std::span<const ParamInfo> params() const noexcept override { return params_.info(); }
    double paramDefault(std::size_t i) const noexcept override { return params_.defaultValue(i); }
    double getParam(std::size_t i) const noexcept override     { return params_.get(i); }
    bool   setParam(std::size_t i, double v) noexcept override { return params_.set(i, v); }

private:
    static float shape(float x) noexcept;
    float driveTarget() const noexcept;
    float mixTarget() const noexcept;

    ParamBlock<3> params_;
    ParamSmoother driveSmoother_;
    ParamSmoother mixSmoother_;

    std::array<Oversampler, kMaxChannels> os_{};
    std::vector<float> up_;   // one channel at 4x, reused across channels
};

} // namespace rt
