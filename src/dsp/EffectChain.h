#pragma once
#include "dsp/IEffect.h"
#include <memory>
#include <vector>

namespace rt {

// Ordered list of effects processed in series, in place.
// add() is NOT realtime safe -- build the chain before start(), or swap a
// whole prepared chain in via a command queue.
class EffectChain {
public:
    void add(std::unique_ptr<IEffect> fx);

    void prepare(double sampleRate, FrameCount maxBlockFrames, int numChannels);
    void reset();
    void process(AudioBufferView& io) noexcept;

    FrameCount totalLatencyFrames() const noexcept;
    std::size_t size() const noexcept { return effects_.size(); }
    IEffect* at(std::size_t i) noexcept { return effects_[i].get(); }

private:
    std::vector<std::unique_ptr<IEffect>> effects_;
};

} // namespace rt
