#pragma once
#include <algorithm>
#include <cstddef>

namespace rt {

// Turns capture-ring fill level into a resampler ratio trim.
//
// Capture and render run off separate crystals, so even at identical nominal
// rates the ring drifts monotonically toward full or empty. Fill level is the
// only observable that says which way, and how fast.
//
// Returning trim > 1 when the ring is over-full is not a sign error: the
// resampler consumes `ratio` input frames per output frame, so a larger ratio
// emits fewer frames and the ring drains.
class DriftController {
public:
    // `targetFill` is the level to hold, in samples. `maxTrim` clamps the
    // correction (0.002 = +/-0.2%), and exists to catch a broken device rather
    // than as a working range -- real crystals sit inside +/-100 ppm.
    void prepare(std::size_t targetFill, double maxTrim) noexcept {
        targetFill_ = static_cast<double>(targetFill);
        maxTrim_    = maxTrim;
        integrator_ = 0.0;
        trim_       = 1.0;
        smoothed_   = targetFill_;   // start settled; see the windup note below
    }

    // Multiplier on the nominal ratio; 1.0 means no correction. Called once per
    // capture packet from the audio thread: no allocation, no locks, no logging.
    double update(std::size_t fill) noexcept {
        if (targetFill_ <= 0.0) return 1.0;

        // Fill jumps by a whole packet on every callback and most of that is
        // scheduling jitter, not drift. Reacting to it would modulate pitch at
        // the callback rate, which is audible as warble, so the loop sees a
        // heavily smoothed estimate instead of the raw level.
        smoothed_ += kSmoothing * (static_cast<double>(fill) - smoothed_);

        const double error = (smoothed_ - targetFill_) / targetFill_;

        // Integral is what actually removes the drift: a pure P controller
        // settles at a permanent fill offset, because zero error would mean
        // zero correction and the crystals would resume diverging.
        integrator_ = std::clamp(integrator_ + kIntegral * error, -maxTrim_, maxTrim_);

        trim_ = 1.0 + std::clamp(kProportional * error + integrator_, -maxTrim_, maxTrim_);
        return trim_;
    }

    double trim() const noexcept { return trim_; }
    double smoothedFill() const noexcept { return smoothed_; }

private:
    // Ring fill is itself an integrator, so this is PI around a double
    // integrator: the proportional term MUST cross over well below the
    // smoothing pole, and the integral zero well below that again, or the loop
    // limit-cycles between the clamps instead of settling.
    //
    //   smoothing pole  1/100 ticks   = 1e-2
    //   P crossover     kP * fill/tick ~ 2.5e-4 .. 2.5e-3   (>= 4x margin)
    //   integral zero   kI / kP        = 5e-5               (>= 10x below that)
    //
    // The middle term varies with packet size relative to ring size, which is
    // why the margins are generous rather than tight.
    static constexpr double kSmoothing    = 1.0e-2;
    static constexpr double kProportional = 5.0e-3;
    static constexpr double kIntegral     = 2.5e-7;

    double targetFill_ = 0.0;
    double maxTrim_    = 0.0;
    double integrator_ = 0.0;
    double trim_       = 1.0;
    double smoothed_   = 0.0;
};

} // namespace rt
