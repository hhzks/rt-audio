#include "dsp/NoiseGate.h"
#include <cmath>

namespace rt {

static inline float msToCoeff(double ms, double sr) {
    const double samples = (ms * 0.001) * sr;
    return samples > 0.0 ? static_cast<float>(std::exp(-1.0 / samples)) : 0.0f;
}

void NoiseGate::prepare(double sampleRate, FrameCount, int) {
    sampleRate_   = sampleRate;
    envCoeff_     = msToCoeff(5.0,   sampleRate); // envelope follower
    attackCoeff_  = msToCoeff(2.0,   sampleRate); // opens fast
    releaseCoeff_ = msToCoeff(120.0, sampleRate); // closes slowly, avoids chatter
    reset();
}

void NoiseGate::reset() { env_.fill(0.0f); gain_.fill(1.0f); }

void NoiseGate::process(AudioBufferView& io) noexcept {
    const float thresholdDb = params_.gateThresholdDb.load(std::memory_order_relaxed);
    const float threshold   = std::pow(10.0f, thresholdDb / 20.0f);
    const float floorGain   = 0.05f; // -26 dB: never fully mute, that sounds worse

    const FrameCount n = io.numFrames();
    for (int ch = 0; ch < io.numChannels(); ++ch) {
        float* x   = io.channel(ch);
        float  env = env_[idx(ch)];
        float  g   = gain_[idx(ch)];

        for (FrameCount i = 0; i < n; ++i) {
            const float a = std::fabs(x[i]);
            env = a + (env - a) * envCoeff_;

            const float targetGain = (env > threshold) ? 1.0f : floorGain;
            const float coeff = (targetGain > g) ? attackCoeff_ : releaseCoeff_;
            g = targetGain + (g - targetGain) * coeff;

            x[i] *= g;
        }
        env_[idx(ch)]  = env;
        gain_[idx(ch)] = g;
    }
}

} // namespace rt
