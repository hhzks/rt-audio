#include "engine/AudioEngine.h"
#include "engine/RtStats.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <vector>

using namespace rt;
using Catch::Matchers::WithinAbs;

TEST_CASE("the callback peak keeps the largest time", "[engine]") {
    RtStats stats;
    stats.recordCallback(100);
    stats.recordCallback(300);
    stats.recordCallback(200);
    CHECK(stats.peakCallbackNanos.load() == 300);
    stats.resetPeaks();
    stats.recordCallback(50);
    CHECK(stats.peakCallbackNanos.load() == 50);
}

TEST_CASE("a second prepare gives the channel count of the last prepare", "[engine]") {
    AudioEngine engine;
    engine.prepare(48000.0, 64, 2);
    engine.prepare(48000.0, 64, 1);
    std::vector<float> in(64, 0.25f), out(64, 0.0f);
    engine.processInterleaved(in.data(), out.data(), 64);
    CHECK(engine.numChannels() == 1);
    CHECK(engine.stats().xruns.load() == 0);
    CHECK_THAT(out[0], WithinAbs(0.25, 1e-6));
    CHECK_THAT(out[63], WithinAbs(0.25, 1e-6));
}
