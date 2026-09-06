#include "engine/LoopbackProbe.h"
#include "TestHarness.h"

#include <cmath>
#include <vector>

using namespace rt;

namespace {
int zeroCrossings(const std::vector<float>& v, std::size_t from, std::size_t to) {
    int n = 0;
    for (std::size_t i = from + 1; i < to; ++i)
        if ((v[i - 1] < 0.0f) != (v[i] < 0.0f)) ++n;
    return n;
}
}

void testSweepLengthAndBounds() {
    SweepConfig cfg;
    std::vector<float> s;
    generateSweep(cfg, 48000.0, s);
    CHECK(s.size() == static_cast<std::size_t>(cfg.seconds * 48000.0));
    for (float v : s) CHECK(std::fabs(v) <= cfg.amplitude + 1e-6f);
}

void testSweepFadesToZero() {
    SweepConfig cfg;
    std::vector<float> s;
    generateSweep(cfg, 48000.0, s);
    CHECK(std::fabs(s.front()) < 1e-4f);
    CHECK(std::fabs(s.back())  < 1e-4f);
}

void testSweepRisesInFrequency() {
    SweepConfig cfg;
    std::vector<float> s;
    generateSweep(cfg, 48000.0, s);
    const std::size_t tenth = s.size() / 10;
    const int early = zeroCrossings(s, tenth, 2 * tenth);
    const int late  = zeroCrossings(s, 8 * tenth, 9 * tenth);
    CHECK(late > early * 4);
}

int main() {
    RUN(testSweepLengthAndBounds);
    RUN(testSweepFadesToZero);
    RUN(testSweepRisesInFrequency);
    TEST_MAIN_END
}
