#include "dsp/Oversampler.h"
#include <cmath>
#include <numbers>
#include <numeric>
#include <algorithm>
#include <ranges>

namespace rt {

namespace {

double sinc(double x) noexcept {
    if (x == 0.0) return 1.0;
    const double px = std::numbers::pi * x;
    return std::sin(px) / px;
}

double idealLowpass(int n){
    constexpr double fc = 0.5 / Oversampler::kRatio;
    constexpr int centre = (Oversampler::kTaps - 1) / 2;
    const double d = static_cast<double>(n - centre);
    return 2.0 * fc * sinc(2.0 * fc * d);

}

double window(int n){
    const double a = 2.0 * std::numbers::pi * static_cast<double>(n)
                    / static_cast<double>(Oversampler::kTaps - 1);
    return 0.42 - 0.5 * std::cos(a) + 0.08 * std::cos(2.0 * a);      //Blackman
}

} // namespace

void Oversampler::designKernel() noexcept {
    std::ranges::transform(std::views::iota(0, kTaps), 
                           kernel_.begin(),
                           [](int n){return static_cast<float>(idealLowpass(n) * window(n));});
    double sum = std::accumulate(kernel_.begin(), kernel_.end(), 0.0);
    std::ranges::transform(kernel_, kernel_.begin(), [sum](float t){return static_cast<float>(static_cast<double>(t) / sum);}); 
}

void Oversampler::prepare() {
    designKernel();

    // Polyphase decomposition
    for (auto& phase : phases_) phase.fill(0.0f);
    for (int k = 0; k < kTaps; ++k)
        phases_[idx(k % kRatio)][idx(k / kRatio)] =
            kernel_[idx(k)] * static_cast<float>(kRatio);

    reset();
}

void Oversampler::reset() noexcept {
    upHist_.fill(0.0f);
    downHist_.fill(0.0f);
    upPos_   = kPhaseTaps - 1;
    downPos_ = kTaps - 1;
}

void Oversampler::upsample(const float* in, float* out, FrameCount n) noexcept {
    for (FrameCount i = 0; i < n; ++i) {
        pushUp(in[i]);
        const float* x = upHist_.data() + upPos_ + kPhaseTaps;

        for (int p = 0; p < kRatio; ++p) {
            const auto& phase = phases_[idx(p)];
            float acc = 0.0f;
            for (int k = 0; k < kPhaseTaps; ++k) acc += phase[idx(k)] * x[-k];
            out[idx(i * kRatio + p)] = acc;
        }
    }
}

void Oversampler::downsample(const float* in, float* out, FrameCount n) noexcept {
    for (FrameCount i = 0; i < n; ++i) {
        const float* group = in + idx(i * kRatio);

        // dot product taken for alignment
        pushDown(group[0]);
        const float* u = downHist_.data() + downPos_ + kTaps;
        float acc = 0.0f;
        for (int k = 0; k < kTaps; ++k) acc += kernel_[idx(k)] * u[-k];
        out[i] = acc;

        for (int p = 1; p < kRatio; ++p) pushDown(group[p]);
    }
}

} // namespace rt
