#include "dsp/Biquad.h"
#include <cmath>

namespace rt {

void Biquad::prepare(double sampleRate, FrameCount, int numChannels) {
    sampleRate_  = sampleRate;
    numChannels_ = numChannels;
    updateCoeffs();
    reset();
}

void Biquad::reset() { state_.fill(State{}); }

void Biquad::setParams(double freqHz, double q, double gainDb) {
    freq_ = freqHz; q_ = q; gainDb_ = gainDb;
    updateCoeffs();
}

void Biquad::updateCoeffs() {
    const double w0    = 2.0 * 3.14159265358979323846 * freq_ / sampleRate_;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q_);
    const double A     = std::pow(10.0, gainDb_ / 40.0);

    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

    switch (type_) {
    case Type::LowPass:
        b0 = (1 - cosw0) / 2; b1 = 1 - cosw0; b2 = b0;
        a0 = 1 + alpha; a1 = -2 * cosw0; a2 = 1 - alpha;
        break;
    case Type::HighPass:
        b0 = (1 + cosw0) / 2; b1 = -(1 + cosw0); b2 = b0;
        a0 = 1 + alpha; a1 = -2 * cosw0; a2 = 1 - alpha;
        break;
    case Type::Peak:
        b0 = 1 + alpha * A; b1 = -2 * cosw0; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cosw0; a2 = 1 - alpha / A;
        break;
    case Type::LowShelf: {
        const double s = 2.0 * std::sqrt(A) * alpha;
        b0 =     A * ((A + 1) - (A - 1) * cosw0 + s);
        b1 = 2 * A * ((A - 1) - (A + 1) * cosw0);
        b2 =     A * ((A + 1) - (A - 1) * cosw0 - s);
        a0 =          (A + 1) + (A - 1) * cosw0 + s;
        a1 =    -2 * ((A - 1) + (A + 1) * cosw0);
        a2 =          (A + 1) + (A - 1) * cosw0 - s;
        break;
    }
    case Type::HighShelf: {
        const double s = 2.0 * std::sqrt(A) * alpha;
        b0 =      A * ((A + 1) + (A - 1) * cosw0 + s);
        b1 = -2 * A * ((A - 1) + (A + 1) * cosw0);
        b2 =      A * ((A + 1) + (A - 1) * cosw0 - s);
        a0 =           (A + 1) - (A - 1) * cosw0 + s;
        a1 =      2 * ((A - 1) - (A + 1) * cosw0);
        a2 =           (A + 1) - (A - 1) * cosw0 - s;
        break;
    }
    }

    b0_ = b0 / a0; b1_ = b1 / a0; b2_ = b2 / a0;
    a1_ = a1 / a0; a2_ = a2 / a0;
}

void Biquad::process(AudioBufferView& io) noexcept {
    const FrameCount n = io.numFrames();
    for (int ch = 0; ch < io.numChannels(); ++ch) {
        float* x = io.channel(ch);
        double z1 = state_[idx(ch)].z1, z2 = state_[idx(ch)].z2;
        for (FrameCount i = 0; i < n; ++i) {
            const double in  = x[i];
            const double out = b0_ * in + z1;
            z1 = b1_ * in - a1_ * out + z2;
            z2 = b2_ * in - a2_ * out;
            x[i] = static_cast<float>(out);
        }
        state_[idx(ch)].z1 = z1;
        state_[idx(ch)].z2 = z2;
    }
}

} // namespace rt
