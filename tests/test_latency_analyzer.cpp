#include "engine/LatencyAnalyzer.h"
#include "engine/LoopbackProbe.h"
#include "core/Types.h"
#include "core/AudioBufferView.h"
#include "dsp/Biquad.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <vector>

using namespace rt;
using Catch::Matchers::WithinAbs;

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

TEST_CASE("clean delay is recovered", "[engine]") {
    const auto ref = makeReference();
    for (int D : {0, 1, 47, 480, 4800}) {
        CAPTURE(D);
        const auto cap = delayedCopy(ref, D);
        const auto r = analyzeLatency(ref, cap, 9600, 48000.0);
        CHECK(r.valid);
        CHECK_THAT(r.lagFrames, WithinAbs(D, 0.5));
        CHECK_FALSE(r.polarityInverted);
        CHECK(r.peakCorrelation > 0.99);
        CHECK(r.peakCorrelation < 1.001);
    }
}

TEST_CASE("attenuation does not matter", "[engine]") {
    const auto ref = makeReference();
    auto cap = delayedCopy(ref, 512);
    for (auto& v : cap) v *= 0.01f;
    const auto r = analyzeLatency(ref, cap, 9600, 48000.0);
    CHECK(r.valid);
    CHECK_THAT(r.lagFrames, WithinAbs(512, 0.5));
    CHECK(r.peakCorrelation > 0.99);
    CHECK(r.peakCorrelation < 1.001);
}

TEST_CASE("polarity inversion is detected", "[engine]") {
    const auto ref = makeReference();
    auto cap = delayedCopy(ref, 512);
    for (auto& v : cap) v = -v;
    const auto r = analyzeLatency(ref, cap, 9600, 48000.0);
    CHECK(r.valid);
    CHECK_THAT(r.lagFrames, WithinAbs(512, 0.5));
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

TEST_CASE("survives noise", "[engine]") {
    const auto ref = makeReference();
    double prevPsr = 1e9;
    for (float noise : {0.05f, 0.15f, 0.5f}) {
        CAPTURE(noise);
        auto cap = delayedCopy(ref, 512);
        addNoise(cap, noise, 12345u);
        const auto r = analyzeLatency(ref, cap, 9600, 48000.0);
        CHECK(r.valid);
        CHECK_THAT(r.lagFrames, WithinAbs(512, 1.0));
        CHECK(r.peakToSidelobe < prevPsr);
        prevPsr = r.peakToSidelobe;
    }
}

TEST_CASE("half-sample delay", "[engine]") {
    const auto ref = makeReference();
    const auto whole = delayedCopy(ref, 512);
    std::vector<float> cap(whole.size(), 0.0f);
    for (std::size_t i = 1; i < whole.size(); ++i)
        cap[i] = 0.5f * (whole[i] + whole[i - 1]);
    const auto r = analyzeLatency(ref, cap, 9600, 48000.0);
    CHECK(r.valid);
    CHECK_THAT(r.lagFrames, WithinAbs(512.5, 0.25));
}

TEST_CASE("band-limited signal is still detected", "[engine]") {
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

TEST_CASE("pure noise is rejected", "[engine]") {
    const auto ref = makeReference();
    std::vector<float> cap(ref.size() + 9600, 0.0f);
    addNoise(cap, 0.5f, 999u);
    const auto r = analyzeLatency(ref, cap, 9600, 48000.0);
    CHECK_FALSE(r.valid);
    CHECK_FALSE(r.rejectReason.empty());
}

TEST_CASE("competing peak is rejected", "[engine]") {
    const auto ref = makeReference();
    std::vector<float> cap(ref.size() + 9600, 0.0f);
    for (std::size_t i = 0; i < ref.size(); ++i) {
        cap[300 + i] += ref[i];
        cap[800 + i] += 0.9f * ref[i];
    }
    const auto r = analyzeLatency(ref, cap, 9600, 48000.0);
    CHECK_FALSE(r.valid);
    CHECK(r.rejectReason == "peak-to-sidelobe below 3.0");
}
