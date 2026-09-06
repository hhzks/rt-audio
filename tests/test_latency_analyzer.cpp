#include "engine/LatencyAnalyzer.h"
#include "engine/LoopbackProbe.h"
#include "core/Types.h"
#include "TestHarness.h"

#include <cstdint>
#include <vector>

using namespace rt;

namespace {
std::vector<float> makeReference() {
    SweepConfig cfg;
    cfg.seconds = 0.1;                 // shorter keeps the test fast
    std::vector<float> ref;
    generateSweep(cfg, 48000.0, ref);
    return ref;
}

std::vector<float> delayedCopy(const std::vector<float>& ref, int D) {
    std::vector<float> cap(ref.size() + static_cast<std::size_t>(D) + 4800, 0.0f);
    for (std::size_t i = 0; i < ref.size(); ++i) cap[idx(D) + i] = ref[i];
    return cap;
}
}

void testCleanDelayRecovered() {
    const auto ref = makeReference();
    for (int D : {0, 1, 47, 480, 4800}) {
        const auto cap = delayedCopy(ref, D);
        const auto r = analyzeLatency(ref, cap, 9600, 48000.0);
        CHECK(r.valid);
        CHECK_NEAR(r.lagFrames, D, 0.5);
        CHECK(!r.polarityInverted);
        CHECK(r.peakCorrelation > 0.99);
        CHECK(r.peakCorrelation < 1.001);
    }
}

void testAttenuationDoesNotMatter() {
    const auto ref = makeReference();
    auto cap = delayedCopy(ref, 512);
    for (auto& v : cap) v *= 0.01f;
    const auto r = analyzeLatency(ref, cap, 9600, 48000.0);
    CHECK(r.valid);
    CHECK_NEAR(r.lagFrames, 512, 0.5);
    CHECK(r.peakCorrelation > 0.99);
    CHECK(r.peakCorrelation < 1.001);
}

void testPolarityInversionDetected() {
    const auto ref = makeReference();
    auto cap = delayedCopy(ref, 512);
    for (auto& v : cap) v = -v;
    const auto r = analyzeLatency(ref, cap, 9600, 48000.0);
    CHECK(r.valid);
    CHECK_NEAR(r.lagFrames, 512, 0.5);
    CHECK(r.polarityInverted);
}

int main() {
    RUN(testCleanDelayRecovered);
    RUN(testAttenuationDoesNotMatter);
    RUN(testPolarityInversionDetected);
    TEST_MAIN_END
}
