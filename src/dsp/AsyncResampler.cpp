#include "dsp/AsyncResampler.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace rt {

namespace {

double sinc(double x) noexcept {
    if (x == 0.0) return 1.0;
    const double px = std::numbers::pi * x;
    return std::sin(px) / px;
}

double blackman(int n, int length) noexcept {
    const double a = 2.0 * std::numbers::pi * static_cast<double>(n)
                   / static_cast<double>(length - 1);
    return 0.42 - 0.5 * std::cos(a) + 0.08 * std::cos(2.0 * a);
}

} // namespace

void AsyncResampler::designTable(double ratio) {
    constexpr int kProtoLen = kTaps * kPhases + 1;
    constexpr int kCentre   = kTaps * kPhases / 2;

    // Band-limit for the LOWER of the two rates. When ratio > 1 we are
    // decimating, and a cutoff at the input Nyquist would fold aliases back in.
    const double fc = 0.5 / std::max(1.0, ratio);

    std::vector<double> proto(static_cast<std::size_t>(kProtoLen));
    for (int k = 0; k < kProtoLen; ++k) {
        const double t = static_cast<double>(k - kCentre) / static_cast<double>(kPhases);
        proto[static_cast<std::size_t>(k)] = 2.0 * fc * sinc(2.0 * fc * t) * blackman(k, kProtoLen);
    }

    table_.assign(idx(kPhases + 1) * idx(kTaps), 0.0f);
    for (int p = 0; p <= kPhases; ++p) {
        // Tap j multiplies the input sample j frames back from the newest.
        double sum = 0.0;
        for (int j = 0; j < kTaps; ++j)
            sum += proto[static_cast<std::size_t>(j * kPhases + p)];

        // Per-row normalisation: every phase must pass DC at unity, or the
        // signal gains a ripple at the rate the phase cycles.
        const double norm = (sum != 0.0) ? 1.0 / sum : 1.0;
        for (int j = 0; j < kTaps; ++j)
            table_[idx(p) * idx(kTaps) + idx(j)] =
                static_cast<float>(proto[static_cast<std::size_t>(j * kPhases + p)] * norm);
    }
}

void AsyncResampler::prepare(int channels, double nominalRatio) {
    channels_ = std::clamp(channels, 1, kMaxChannels);
    ratio_    = nominalRatio;
    hist_.assign(idx(channels_) * idx(2 * kTaps), 0.0f);
    designTable(nominalRatio);
    reset();
}

void AsyncResampler::reset() noexcept {
    std::fill(hist_.begin(), hist_.end(), 0.0f);
    pos_   = kTaps - 1;
    phase_ = 1.0;
}

FrameCount AsyncResampler::maxOutputFor(FrameCount inFrames, double ratio) noexcept {
    const double n = std::ceil(static_cast<double>(inFrames) / std::max(ratio, 1e-6));
    return static_cast<FrameCount>(n) + 2;
}

void AsyncResampler::pushFrame(const float* frame) noexcept {
    pos_ = (pos_ + 1 == kTaps) ? 0 : pos_ + 1;
    for (int c = 0; c < channels_; ++c) {
        float* base = hist_.data() + idx(c) * idx(2 * kTaps);
        base[idx(pos_)]          = frame[idx(c)];
        base[idx(pos_ + kTaps)]  = frame[idx(c)];
    }
}

FrameCount AsyncResampler::process(const float* in, FrameCount inFrames,
                                   float* out, FrameCount maxOutFrames) noexcept {
    const int  ch       = channels_;
    FrameCount produced = 0;
    FrameCount consumed = 0;

    for (;;) {
        while (phase_ >= 1.0) {
            if (consumed == inFrames) return produced;
            pushFrame(in + idx(consumed) * idx(ch));
            ++consumed;
            phase_ -= 1.0;
        }
        if (produced == maxOutFrames) return produced;

        const double p  = phase_ * static_cast<double>(kPhases);
        const int    p0 = static_cast<int>(p);
        const float  f  = static_cast<float>(p - static_cast<double>(p0));

        const float* a = table_.data() + idx(p0) * idx(kTaps);
        const float* b = a + kTaps;

        float* dst = out + idx(produced) * idx(ch);
        for (int c = 0; c < ch; ++c) {
            // Walking backwards from the newest sample is contiguous because
            // every sample is stored twice; same trick as Oversampler.
            const float* h = hist_.data() + idx(c) * idx(2 * kTaps) + idx(pos_ + kTaps);
            float sa = 0.0f, sb = 0.0f;
            for (int j = 0; j < kTaps; ++j) {
                const float x = h[-j];
                sa += a[idx(j)] * x;
                sb += b[idx(j)] * x;
            }
            dst[idx(c)] = sa + f * (sb - sa);
        }

        ++produced;
        phase_ += ratio_;
    }
}

} // namespace rt
