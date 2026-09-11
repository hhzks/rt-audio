#include "dsp/NoiseGate.h"
#include <algorithm>
#include <cmath>

namespace rt {

namespace {

constexpr std::array<ParamInfo, 3> kGateInfo{{
    {"on",             "on",  "",     0.0, 1.0,   1.0, Taper::Linear, kToggle},
    {"threshold",      "thr", "dB", -90.0, 0.0, -45.0, Taper::Linear, 0},
    {"gain_reduction", "gr",  "dB", -26.0, 0.0,   0.0, Taper::Linear, kReadOnly},
}};

float msToCoeff(double ms, double sr) {
    const double samples = (ms * 0.001) * sr;
    return samples > 0.0 ? static_cast<float>(std::exp(-1.0 / samples)) : 0.0f;
}

} // namespace

NoiseGate::NoiseGate() : params_(kGateInfo) {}

void NoiseGate::prepare(double sampleRate, FrameCount, int) {
    sampleRate_   = sampleRate;
    envCoeff_     = msToCoeff(5.0,   sampleRate); // envelope follower
    attackCoeff_  = msToCoeff(2.0,   sampleRate); // opens fast
    releaseCoeff_ = msToCoeff(120.0, sampleRate); // closes slowly, avoids chatter
    reset();
}

void NoiseGate::reset() {
    env_.fill(0.0f);
    gain_.fill(1.0f);
    params_.publish(kGainReduction, 0.0);
}

void NoiseGate::process(AudioBufferView& io) noexcept {
    const bool  on          = params_.get(kOn) >= 0.5;
    const float thresholdDb = static_cast<float>(params_.get(kThreshold));
    const float threshold   = std::pow(10.0f, thresholdDb / 20.0f);
    const float floorGain   = 0.05f; // -26 dB: never fully mute, that sounds worse

    float minGain = 1.0f;
    const FrameCount n = io.numFrames();
    for (int ch = 0; ch < io.numChannels(); ++ch) {
        float* x   = io.channel(ch);
        float  env = env_[idx(ch)];
        float  g   = gain_[idx(ch)];

        for (FrameCount i = 0; i < n; ++i) {
            const float a = std::fabs(x[i]);
            env = a + (env - a) * envCoeff_;

            const float targetGain = (!on || env > threshold) ? 1.0f : floorGain;
            const float coeff = (targetGain > g) ? attackCoeff_ : releaseCoeff_;
            g = targetGain + (g - targetGain) * coeff;

            x[i] *= g;
        }
        env_[idx(ch)]  = env;
        gain_[idx(ch)] = g;
        minGain = std::min(minGain, g);
    }
    params_.publish(kGainReduction,
                    std::max(-26.0, 20.0 * std::log10(static_cast<double>(minGain))));
}

} // namespace rt
