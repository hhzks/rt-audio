#pragma once
#include <cmath>

namespace rt {

// One-pole ramp toward a target. Without this, every parameter change is a
// step discontinuity in the signal -- audible as a click ("zipper noise").
class ParamSmoother {
public:
    void prepare(double sampleRate, double rampMs, float initial) noexcept {
        // coeff such that we cover ~63% of the remaining distance in rampMs
        const double samples = (rampMs * 0.001) * sampleRate;
        coeff_   = samples > 0.0 ? static_cast<float>(std::exp(-1.0 / samples)) : 0.0f;
        current_ = initial;
        target_  = initial;
    }

    void setTarget(float v) noexcept { target_ = v; }
    void snapTo(float v)    noexcept { target_ = current_ = v; }

    float next() noexcept {
        current_ = target_ + (current_ - target_) * coeff_;
        return current_;
    }

    float current() const noexcept { return current_; }
    bool  isSmoothing() const noexcept { return std::fabs(current_ - target_) > 1e-6f; }

private:
    float coeff_ = 0.0f, current_ = 0.0f, target_ = 0.0f;
};

} // namespace rt
