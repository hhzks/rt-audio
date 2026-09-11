#include "dsp/Biquad.h"
#include <cmath>

namespace rt {

namespace {

constexpr std::array<ParamInfo, 3> kBiquadInfo{{
    {"freq", "freq", "Hz",  20.0, 20000.0, 1000.0, Taper::Log,    0},
    {"q",    "Q",    "",     0.1,    10.0,  0.707, Taper::Log,    0},
    {"gain", "gain", "dB", -24.0,    24.0,    0.0, Taper::Linear, 0},
}};

bool hasGain(Biquad::Type t) noexcept {
    return t == Biquad::Type::Peak || t == Biquad::Type::LowShelf
        || t == Biquad::Type::HighShelf;
}

} // namespace

Biquad::Biquad(Type type, double freqHz, double q, double gainDb)
    : type_(type), params_(kBiquadInfo), freq_(freqHz), q_(q), gainDb_(gainDb) {
    params_.setDefault(kFreq, freqHz);
    params_.setDefault(kQ, q);
    params_.setDefault(kGain, gainDb);
}

void Biquad::prepare(double sampleRate, FrameCount, int numChannels) {
    sampleRate_  = sampleRate;
    numChannels_ = numChannels;
    updateCoeffs();
    reset();
}

void Biquad::reset() { state_.fill(State{}); }

const char* Biquad::name() const noexcept {
    switch (type_) {
    case Type::LowPass:   return "Low-pass";
    case Type::HighPass:  return "High-pass";
    case Type::Peak:      return "Peak";
    case Type::LowShelf:  return "Low shelf";
    case Type::HighShelf: return "High shelf";
    }
    return "Biquad";
}

std::span<const ParamInfo> Biquad::params() const noexcept {
    return params_.info().first(hasGain(type_) ? 3 : 2);
}

bool Biquad::setParam(std::size_t i, double v) noexcept {
    if (i >= params().size()) return false;
    return params_.set(i, v);
}

void Biquad::updateCoeffs() noexcept {
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
    const double f = params_.get(kFreq), q = params_.get(kQ), g = params_.get(kGain);
    if (f != freq_ || q != q_ || g != gainDb_) {
        freq_ = f; q_ = q; gainDb_ = g;
        updateCoeffs();
    }

    const FrameCount n = io.numFrames();
    for (int ch = 0; ch < io.numChannels(); ++ch) {
        float* x = io.channel(ch);
        double z1 = state_[idx(ch)].z1, z2 = state_[idx(ch)].z2;
        for (FrameCount i = 0; i < n; ++i) {
            const double in  = static_cast<double>(x[i]);
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
