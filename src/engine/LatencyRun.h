#pragma once
#include "engine/LatencyAnalyzer.h"
#include "engine/LoopbackProbe.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>

namespace rt {

struct PhaseConfig {
    int    repeats      = 5;
    int    maxRetries   = 3;
    int    maxLagFrames = 0;
    double sampleRate   = 48000.0;
    std::chrono::milliseconds timeout{10000};
};

enum class PhaseEvent { Attempt, Discarded, Clipped };

struct PhaseHooks {
    std::function<std::uint64_t()> dropouts;
    std::function<bool()>          cancelled;
    std::function<bool()>          alive;
    std::function<void(PhaseEvent, int repeat, int retry)> progress;
};

struct PhaseResult {
    std::vector<LatencyResult> kept;
    int  discarded  = 0;
    int  requested  = 0;
    bool anyClipped = false;

    int    validCount() const;
    bool   anyValid() const;
    bool   enoughKept() const;
    bool   passedMajority() const;
    double medianMs() const;
    double spreadMs() const;
    const LatencyResult* representative() const;
};

class PhaseCancelled : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Not real-time. Arms the probe once per attempt; the probe must be Idle or Done on entry.
PhaseResult runPhase(LoopbackProbe& probe, const PhaseConfig& cfg, const PhaseHooks& hooks);

} // namespace rt
