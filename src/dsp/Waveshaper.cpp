#include "dsp/Waveshaper.h"
#include <cmath>

namespace rt {

void Waveshaper::prepare(double sampleRate, FrameCount, int) {
    driveSmoother_.prepare(sampleRate, 20.0, params_.drive.load(std::memory_order_relaxed));
    mixSmoother_.prepare(sampleRate, 20.0, params_.mix.load(std::memory_order_relaxed));
}

void Waveshaper::reset() {
    driveSmoother_.snapTo(params_.drive.load(std::memory_order_relaxed));
    mixSmoother_.snapTo(params_.mix.load(std::memory_order_relaxed));
}

float Waveshaper::shape(float x) noexcept {
    // tanh-ish soft clip. std::tanh is accurate but slow-ish; this rational
    // approximation is monotonic, odd-symmetric and about 5x cheaper.
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

void Waveshaper::process(AudioBufferView& io) noexcept {
    // Read the atomics ONCE per block, not per sample: relaxed atomic loads in
    // an inner loop defeat vectorisation.
    driveSmoother_.setTarget(params_.drive.load(std::memory_order_relaxed));
    mixSmoother_.setTarget(params_.mix.load(std::memory_order_relaxed));

    const FrameCount n = io.numFrames();
    const int channels = io.numChannels();

    // Smoothers advance once per block here. Per-sample smoothing needs the
    // smoother state duplicated per channel -- easy, but keep it obvious first.
    float drive = driveSmoother_.current();
    float mix   = mixSmoother_.current();
    for (FrameCount i = 0; i < n; ++i) { drive = driveSmoother_.next(); mix = mixSmoother_.next(); }

    const float makeup = 1.0f / shape(drive); // keep unity-ish output level

    for (int ch = 0; ch < channels; ++ch) {
        float* x = io.channel(ch);
        for (FrameCount i = 0; i < n; ++i) {
            const float dry = x[i];
            const float wet = shape(dry * drive) * makeup;
            x[i] = dry + mix * (wet - dry);
        }
    }
}

} // namespace rt
