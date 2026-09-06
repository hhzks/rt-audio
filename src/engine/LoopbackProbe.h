#pragma once
#include "core/Types.h"
#include <atomic>
#include <span>
#include <vector>

namespace rt {

struct SweepConfig {
    double loHz              = 200.0;
    double hiHz              = 10000.0;
    double seconds           = 0.4;
    float  amplitude         = 0.5f;
    double fadeSeconds       = 0.005;
    double primeSeconds      = 0.1;
    double maxLatencySeconds = 0.2;
    double tailSeconds       = 0.1;
};

void generateSweep(const SweepConfig& cfg, double sampleRate, std::vector<float>& out);

enum class ProbeState { Idle, Priming, Emitting, Trailing, Done };

class LoopbackProbe {
public:
    void prepare(double sampleRate, FrameCount maxBlockFrames, int numChannels,
                 const SweepConfig& cfg);
    void arm() noexcept;
    void process(const float* in, float* out, FrameCount numFrames) noexcept;

    ProbeState state() const noexcept { return state_.load(std::memory_order_relaxed); }
    std::span<const float> reference() const noexcept { return sweep_; }
    std::span<const float> captured()  const noexcept { return capture_; }

private:
    std::vector<float>      sweep_, capture_;
    std::atomic<ProbeState> state_{ProbeState::Idle};
    std::size_t sweepPos_ = 0, capturePos_ = 0, primeRemaining_ = 0;
    int         numChannels_ = 0;
    FrameCount  maxBlockFrames_ = 0;
    std::size_t primeFrames_ = 0;
    bool        prepared_ = false;
};

} // namespace rt
