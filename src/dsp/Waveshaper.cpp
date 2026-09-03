#include "dsp/Waveshaper.h"
#include <algorithm>
#include <cmath>
#include <ranges>

namespace rt {

void Waveshaper::prepare(double sampleRate, FrameCount maxBlockFrames, int) {
    driveSmoother_.prepare(sampleRate, 20.0, params_.drive.load(std::memory_order_relaxed));
    mixSmoother_.prepare(sampleRate, 20.0, params_.mix.load(std::memory_order_relaxed));

    for (auto& os : os_) os.prepare();
    up_.assign(idx(maxBlockFrames * Oversampler::kRatio), 0.0f);
}

void Waveshaper::reset() {
    driveSmoother_.snapTo(params_.drive.load(std::memory_order_relaxed));
    mixSmoother_.snapTo(params_.mix.load(std::memory_order_relaxed));
    for (auto& os : os_) os.reset();
}

float Waveshaper::shape(float x) noexcept {
    // tanh-ish soft clip approximation (5x cheaper than std::tanh)
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

void Waveshaper::process(AudioBufferView& io) noexcept {
    driveSmoother_.setTarget(params_.drive.load(std::memory_order_relaxed));
    mixSmoother_.setTarget(params_.mix.load(std::memory_order_relaxed));

    const FrameCount n = io.numFrames();
    const int channels = io.numChannels();

    // Smoothers advance once per block here
    float drive = driveSmoother_.current();
    float mix   = mixSmoother_.current();
    for (FrameCount i = 0; i < n; ++i) { drive = driveSmoother_.next(); mix = mixSmoother_.next(); }

    // headroom toi avoid hitten clamp
    constexpr float kHeadroom = 0.84f;
    const float makeup = kHeadroom / shape(drive);

    const FrameCount upCount = n * Oversampler::kRatio;
    if (idx(upCount) > up_.size()) return;

    for (int ch = 0; ch < channels; ++ch) {
        float* x = io.channel(ch);
        os_[idx(ch)].upsample(x, up_.data(), n);

        std::ranges::transform(std::views::take(up_, upCount), 
                               up_.begin(), 
                               [drive, mix, makeup](float dry){float wet = shape(dry * drive) * makeup; 
                                                               return dry + mix * (wet - dry);});

        os_[idx(ch)].downsample(up_.data(), x, n);
    }
}

} // namespace rt
