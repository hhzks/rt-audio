#include "engine/LatencyRun.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <thread>

namespace rt {

namespace {

std::vector<double> validLags(const std::vector<LatencyResult>& kept) {
    std::vector<double> v;
    for (const LatencyResult& r : kept)
        if (r.valid) v.push_back(r.lagMs);
    return v;
}

struct Attempt {
    LatencyResult result;
    bool xrun    = false;
    bool clipped = false;
};

Attempt runOne(LoopbackProbe& probe, const PhaseConfig& cfg, const PhaseHooks& hooks) {
    const std::uint64_t before = hooks.dropouts ? hooks.dropouts() : 0;
    probe.arm();
    const auto deadline = std::chrono::steady_clock::now() + cfg.timeout;
    while (probe.state() != ProbeState::Done) {
        if (hooks.cancelled && hooks.cancelled()) throw PhaseCancelled("cancelled");
        if (hooks.alive && !hooks.alive())
            throw std::runtime_error("the device stopped during the sweep");
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("timed out waiting for the sweep to complete");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const std::uint64_t after = hooks.dropouts ? hooks.dropouts() : 0;

    Attempt a;
    a.xrun = after != before;
    for (float v : probe.captured()) {
        if (std::fabs(v) >= 0.999f) {
            a.clipped = true;
            break;
        }
    }
    a.result = analyzeLatency(probe.reference(), probe.captured(), cfg.maxLagFrames, cfg.sampleRate);
    return a;
}

void keep(PhaseResult& phase, const Attempt& attempt, int repeat, const PhaseHooks& hooks) {
    if (attempt.clipped) {
        phase.anyClipped = true;
        if (hooks.progress) hooks.progress(PhaseEvent::Clipped, repeat, 0);
    }
    phase.kept.push_back(attempt.result);
}

// Sweeps until two consecutive clean lags agree; the confirming sweep becomes repeat 1.
void warmUp(LoopbackProbe& probe, const PhaseConfig& cfg, const PhaseHooks& hooks,
            PhaseResult& phase) {
    std::optional<double> previous;
    for (int sweep = 1; sweep <= cfg.maxWarmup; ++sweep) {
        if (hooks.progress) hooks.progress(PhaseEvent::Warmup, sweep, 0);
        const Attempt a = runOne(probe, cfg, hooks);
        if (a.xrun || !a.result.valid) {
            previous.reset();
            continue;
        }
        if (previous && std::fabs(a.result.lagMs - *previous) <= cfg.settleMs) {
            phase.warmup = sweep - 1;
            keep(phase, a, 1, hooks);
            return;
        }
        previous = a.result.lagMs;
    }
    phase.warmup  = cfg.maxWarmup;
    phase.settled = false;
}

} // namespace

int PhaseResult::validCount() const {
    return static_cast<int>(std::count_if(kept.begin(), kept.end(),
                                          [](const LatencyResult& r) { return r.valid; }));
}

bool PhaseResult::anyValid() const { return validCount() > 0; }

bool PhaseResult::enoughKept() const {
    return !kept.empty() && static_cast<int>(kept.size()) >= std::min(3, requested);
}

bool PhaseResult::passedMajority() const {
    return enoughKept() && validCount() * 2 > static_cast<int>(kept.size());
}

double PhaseResult::medianMs() const {
    std::vector<double> v = validLags(kept);
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double PhaseResult::spreadMs() const {
    const std::vector<double> v = validLags(kept);
    if (v.empty()) return 0.0;
    const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
    return *hi - *lo;
}

const LatencyResult* PhaseResult::representative() const {
    const double m = medianMs();
    const LatencyResult* best = nullptr;
    for (const LatencyResult& r : kept) {
        if (!r.valid) continue;
        if (best == nullptr || std::fabs(r.lagMs - m) < std::fabs(best->lagMs - m)) best = &r;
    }
    return best;
}

PhaseResult runPhase(LoopbackProbe& probe, const PhaseConfig& cfg, const PhaseHooks& hooks) {
    PhaseResult phase;
    phase.requested = cfg.repeats;
    if (cfg.maxWarmup > 0 && cfg.repeats > 0) warmUp(probe, cfg, hooks, phase);
    for (int i = static_cast<int>(phase.kept.size()); i < cfg.repeats; ++i) {
        const int repeat = i + 1;
        Attempt attempt;
        for (int retry = 0;; ++retry) {
            if (hooks.progress) hooks.progress(PhaseEvent::Attempt, repeat, retry);
            attempt = runOne(probe, cfg, hooks);
            if (!attempt.xrun || retry >= cfg.maxRetries) break;
        }
        if (attempt.xrun) {
            ++phase.discarded;
            if (hooks.progress) hooks.progress(PhaseEvent::Discarded, repeat, cfg.maxRetries);
            continue;
        }
        keep(phase, attempt, repeat, hooks);
    }
    return phase;
}

} // namespace rt
