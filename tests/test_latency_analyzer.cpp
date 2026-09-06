#include "engine/LatencyAnalyzer.h"
#include "engine/LoopbackProbe.h"
#include "core/Types.h"
#include "core/AudioBufferView.h"
#include "dsp/Biquad.h"
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

namespace {
void addNoise(std::vector<float>& v, float amp, std::uint32_t seed) {
    std::uint32_t rng = seed;
    for (auto& s : v) {
        rng = rng * 1664525u + 1013904223u;
        s += amp * static_cast<float>((rng >> 8) / 8388608.0 - 1.0);
    }
}
}

void testSurvivesNoise() {
    const auto ref = makeReference();
    double prevPsr = 1e9;
    for (float noise : {0.05f, 0.15f, 0.5f}) {
        auto cap = delayedCopy(ref, 512);
        addNoise(cap, noise, 12345u);
        const auto r = analyzeLatency(ref, cap, 9600, 48000.0);
        CHECK(r.valid);
        CHECK_NEAR(r.lagFrames, 512, 1.0);
        CHECK(r.peakToSidelobe < prevPsr);
        prevPsr = r.peakToSidelobe;
    }
}

void testHalfSampleDelay() {
    const auto ref = makeReference();
    const auto whole = delayedCopy(ref, 512);
    std::vector<float> cap(whole.size(), 0.0f);
    for (std::size_t i = 1; i < whole.size(); ++i)
        cap[i] = 0.5f * (whole[i] + whole[i - 1]);
    const auto r = analyzeLatency(ref, cap, 9600, 48000.0);
    CHECK(r.valid);
    CHECK_NEAR(r.lagFrames, 512.5, 0.25);
}

void testBandlimitedStillDetected() {
    const auto ref = makeReference();
    auto cap = delayedCopy(ref, 512);
    Biquad lp(Biquad::Type::LowPass, 4000.0, 0.707);
    lp.prepare(48000.0, static_cast<FrameCount>(cap.size()), 1);
    float* ch = cap.data();
    AudioBufferView view(&ch, 1, static_cast<FrameCount>(cap.size()));
    lp.process(view);
    const auto r = analyzeLatency(ref, cap, 9600, 48000.0);
    CHECK(r.valid);
    CHECK(r.lagFrames >= 512.0);
    CHECK(r.lagFrames <  520.0);
}

void testPureNoiseRejected() {
    const auto ref = makeReference();
    std::vector<float> cap(ref.size() + 9600, 0.0f);
    addNoise(cap, 0.5f, 999u);
    const auto r = analyzeLatency(ref, cap, 9600, 48000.0);
    CHECK(!r.valid);
    CHECK(!r.rejectReason.empty());
}

int main() {
    RUN(testCleanDelayRecovered);
    RUN(testAttenuationDoesNotMatter);
    RUN(testPolarityInversionDetected);
    RUN(testSurvivesNoise);
    RUN(testHalfSampleDelay);
    RUN(testBandlimitedStillDetected);
    RUN(testPureNoiseRejected);
    TEST_MAIN_END
}
