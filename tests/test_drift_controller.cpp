// Closes the loop in simulation: a fake ring fed by a crystal that is slightly
// off, to prove the controller pulls fill back to target and parks the trim on
// the true rate error instead of oscillating around it.
#include "core/DriftController.h"
#include "TestHarness.h"

#include <cmath>
#include <cstddef>

using namespace rt;

namespace {

constexpr double kTarget   = 4800.0;   // samples
constexpr double kConsumed = 480.0;    // render pops this per tick
constexpr double kTickHz   = 100.0;    // 48 kHz / 480

struct Sim {
    double fill = kTarget;
    double trim = 1.0;
};

// One callback: capture delivers `kConsumed * (1 + drift)` frames, the
// resampler turns them into that many / trim, and render pops kConsumed.
void tick(Sim& s, DriftController& c, double drift) {
    const double delivered = kConsumed * (1.0 + drift);
    s.fill += delivered / s.trim - kConsumed;
    s.fill = std::max(s.fill, 0.0);
    s.trim = c.update(static_cast<std::size_t>(s.fill));
}

void settle(Sim& s, DriftController& c, double drift, int ticks) {
    for (int i = 0; i < ticks; ++i) tick(s, c, drift);
}

} // namespace

void testNoDriftLeavesRatioAlone() {
    DriftController c;
    c.prepare(static_cast<std::size_t>(kTarget), 0.002);
    Sim s;

    settle(s, c, 0.0, 20000);
    CHECK_NEAR(s.trim, 1.0, 1e-9);
    CHECK_NEAR(s.fill, kTarget, 1.0);
}

// 50 ppm is a realistic mismatch between two consumer crystals. Uncorrected it
// drains a 4800-sample ring in about 100 seconds; corrected it must not.
void testConvergesOnTrueRateError() {
    for (const double drift : { 50e-6, -50e-6, 200e-6 }) {
        DriftController c;
        c.prepare(static_cast<std::size_t>(kTarget), 0.002);
        Sim s;

        settle(s, c, drift, static_cast<int>(kTickHz * 600.0));   // 10 minutes

        std::printf("  drift %+7.1f ppm -> trim %+7.1f ppm, fill %.0f\n",
                    drift * 1e6, (s.trim - 1.0) * 1e6, s.fill);

        CHECK_NEAR((s.trim - 1.0) * 1e6, drift * 1e6, 2.0);   // within 2 ppm
        CHECK_NEAR(s.fill, kTarget, kTarget * 0.05);          // within 5% of target
    }
}

// The ring must not run dry or overflow on the way to convergence -- an
// excursion past either end is a dropout the user hears.
void testNoExcursionDuringConvergence() {
    DriftController c;
    c.prepare(static_cast<std::size_t>(kTarget), 0.002);
    Sim s;

    double lo = kTarget, hi = kTarget;
    for (int i = 0; i < static_cast<int>(kTickHz * 600.0); ++i) {
        tick(s, c, 100e-6);
        lo = std::min(lo, s.fill);
        hi = std::max(hi, s.fill);
    }
    std::printf("  fill excursion: %.0f .. %.0f (target %.0f)\n", lo, hi, kTarget);

    CHECK(lo > kTarget * 0.5);
    CHECK(hi < kTarget * 1.5);
}

// A device reporting a wildly wrong rate must not let the trim run away; the
// clamp is the difference between degraded audio and a pitch-shifted mess.
void testTrimIsClamped() {
    DriftController c;
    c.prepare(static_cast<std::size_t>(kTarget), 0.002);

    for (int i = 0; i < 100000; ++i) c.update(0);
    CHECK(c.trim() >= 1.0 - 0.002 - 1e-12);

    DriftController c2;
    c2.prepare(static_cast<std::size_t>(kTarget), 0.002);
    for (int i = 0; i < 100000; ++i) c2.update(static_cast<std::size_t>(kTarget) * 100);
    CHECK(c2.trim() <= 1.0 + 0.002 + 1e-12);
}

int main() {
    RUN(testNoDriftLeavesRatioAlone);
    RUN(testConvergesOnTrueRateError);
    RUN(testNoExcursionDuringConvergence);
    RUN(testTrimIsClamped);
    TEST_MAIN_END
}
