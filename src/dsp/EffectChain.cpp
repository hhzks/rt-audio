#include "dsp/EffectChain.h"

namespace rt {

void EffectChain::add(std::unique_ptr<IEffect> fx) {
    effects_.push_back(std::move(fx));
}

void EffectChain::prepare(double sampleRate, FrameCount maxBlockFrames, int numChannels) {
    for (auto& fx : effects_) fx->prepare(sampleRate, maxBlockFrames, numChannels);
}

void EffectChain::reset() {
    for (auto& fx : effects_) fx->reset();
}

void EffectChain::process(AudioBufferView& io) noexcept {
    // Iterating the chain does not allocate. The virtual calls are once per
    // block, not per sample, so the indirection is free in practice.
    for (auto& fx : effects_) fx->process(io);
}

FrameCount EffectChain::totalLatencyFrames() const noexcept {
    FrameCount total = 0;
    for (auto const& fx : effects_) total += fx->latencyFrames();
    return total;
}

} // namespace rt
