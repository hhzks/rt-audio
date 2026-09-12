#include "engine/LatencyRun.h"
#include "engine/LoopbackProbe.h"
#include "DelayLine.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

using namespace rt;
using rt::testing::DelayLine;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace {

SweepConfig shortSweep(float amplitude = 0.5f) {
    SweepConfig cfg;
    cfg.seconds = 0.1; cfg.primeSeconds = 0.02;
    cfg.maxLatencySeconds = 0.05; cfg.tailSeconds = 0.02;
    cfg.amplitude = amplitude;
    return cfg;
}

PhaseConfig phaseConfig(int repeats) {
    PhaseConfig c;
    c.repeats      = repeats;
    c.maxLagFrames = static_cast<int>(0.05 * 48000.0);
    c.sampleRate   = 48000.0;
    return c;
}

// Drives the probe from its own thread through a mono delay line, like a device.
class ProbeLoop {
public:
    ProbeLoop(LoopbackProbe& probe, int delayFrames)
        : probe_(probe), loop_(delayFrames, 1), thread_([this] { run(); }) {}
    ~ProbeLoop() {
        stop_.store(true);
        thread_.join();
    }
    ProbeLoop(const ProbeLoop&) = delete;
    ProbeLoop& operator=(const ProbeLoop&) = delete;

private:
    void run() {
        std::vector<float> in(128), out(128);
        while (!stop_.load()) {
            loop_.read(in.data(), 128);
            probe_.process(in.data(), out.data(), 128);
            loop_.write(out.data(), 128);
            std::this_thread::yield();
        }
    }

    LoopbackProbe&    probe_;
    DelayLine         loop_;
    std::atomic<bool> stop_{false};
    std::thread       thread_;   // last, so the members above exist before it starts
};

LatencyResult valid(double ms) {
    LatencyResult r;
    r.valid = true;
    r.lagMs = ms;
    return r;
}

LatencyResult rejected() { return LatencyResult{}; }

} // namespace

TEST_CASE("runPhase recovers a known delay", "[engine]") {
    LoopbackProbe probe;
    probe.prepare(48000.0, 128, 1, shortSweep());
    ProbeLoop loop(probe, 512);
    const PhaseResult r = runPhase(probe, phaseConfig(3), PhaseHooks{});
    CHECK(r.kept.size() == 3);
    CHECK(r.discarded == 0);
    CHECK(r.passedMajority());
    CHECK_THAT(r.medianMs(), WithinAbs(512.0 / 48.0, 0.5 / 48.0));
}

TEST_CASE("an xrun during a sweep is retried", "[engine]") {
    LoopbackProbe probe;
    probe.prepare(48000.0, 128, 1, shortSweep());
    ProbeLoop loop(probe, 512);

    std::uint64_t calls = 0;
    std::vector<std::pair<PhaseEvent, int>> events;
    PhaseHooks hooks;
    hooks.dropouts = [&] { ++calls; return std::min<std::uint64_t>(calls, 2); };
    hooks.progress = [&](PhaseEvent e, int, int retry) { events.emplace_back(e, retry); };

    const PhaseResult r = runPhase(probe, phaseConfig(1), hooks);
    CHECK(r.kept.size() == 1);
    CHECK(r.discarded == 0);
    REQUIRE(events.size() == 2);
    CHECK(events[0] == std::pair{PhaseEvent::Attempt, 0});
    CHECK(events[1] == std::pair{PhaseEvent::Attempt, 1});
}

TEST_CASE("an xrun that persists discards the repeat", "[engine]") {
    LoopbackProbe probe;
    probe.prepare(48000.0, 128, 1, shortSweep());
    ProbeLoop loop(probe, 512);

    std::uint64_t calls = 0;
    int attempts = 0;
    bool discarded = false;
    PhaseHooks hooks;
    hooks.dropouts = [&] { return ++calls; };
    hooks.progress = [&](PhaseEvent e, int, int) {
        if (e == PhaseEvent::Attempt) ++attempts;
        if (e == PhaseEvent::Discarded) discarded = true;
    };

    const PhaseResult r = runPhase(probe, phaseConfig(1), hooks);
    CHECK(r.kept.empty());
    CHECK(r.discarded == 1);
    CHECK(attempts == 4);
    CHECK(discarded);
}

TEST_CASE("a clipped capture is reported", "[engine]") {
    LoopbackProbe probe;
    probe.prepare(48000.0, 128, 1, shortSweep(1.0f));
    ProbeLoop loop(probe, 512);

    bool clippedEvent = false;
    PhaseHooks hooks;
    hooks.progress = [&](PhaseEvent e, int, int) { if (e == PhaseEvent::Clipped) clippedEvent = true; };
    const PhaseResult r = runPhase(probe, phaseConfig(1), hooks);
    CHECK(r.anyClipped);
    CHECK(clippedEvent);
}

TEST_CASE("runPhase stops on cancel, device loss and timeout", "[engine]") {
    LoopbackProbe probe;   // no thread drives it, so it stays in Priming after arm()

    probe.prepare(48000.0, 128, 1, shortSweep());
    PhaseHooks cancel;
    cancel.cancelled = [] { return true; };
    CHECK_THROWS_AS(runPhase(probe, phaseConfig(1), cancel), PhaseCancelled);

    probe.prepare(48000.0, 128, 1, shortSweep());
    PhaseHooks dead;
    dead.alive = [] { return false; };
    CHECK_THROWS_WITH(runPhase(probe, phaseConfig(1), dead), ContainsSubstring("device stopped"));

    probe.prepare(48000.0, 128, 1, shortSweep());
    PhaseConfig slow = phaseConfig(1);
    slow.timeout = std::chrono::milliseconds(20);
    CHECK_THROWS_WITH(runPhase(probe, slow, PhaseHooks{}), ContainsSubstring("timed out"));
}

TEST_CASE("phase result rules", "[engine]") {
    PhaseResult p;
    p.requested = 5;
    p.kept = {valid(10.0), valid(12.0), rejected()};
    CHECK(p.enoughKept());
    CHECK(p.passedMajority());
    CHECK_THAT(p.medianMs(), WithinAbs(11.0, 1e-12));
    CHECK_THAT(p.spreadMs(), WithinAbs(2.0, 1e-12));
    REQUIRE(p.representative() != nullptr);
    CHECK_THAT(p.representative()->lagMs, WithinAbs(10.0, 1e-12));   // ties go to the first

    p.kept.push_back(rejected());
    CHECK(!p.passedMajority());                                       // 2 valid of 4 is not a majority

    PhaseResult odd;
    odd.requested = 3;
    odd.kept = {valid(3.0), valid(1.0), valid(2.0)};
    CHECK_THAT(odd.medianMs(), WithinAbs(2.0, 1e-12));

    PhaseResult few;
    few.requested = 2;
    few.kept = {valid(1.0)};
    CHECK(!few.enoughKept());                                         // 1 < min(3, 2)

    PhaseResult none;
    none.requested = 5;
    CHECK(!none.enoughKept());
    CHECK(!none.anyValid());
    CHECK(none.medianMs() == 0.0);
    CHECK(none.spreadMs() == 0.0);
    CHECK(none.representative() == nullptr);
}
