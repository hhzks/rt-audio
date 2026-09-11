#include "dsp/Biquad.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <numbers>
#include <vector>

using namespace rt;
using Catch::Matchers::WithinAbs;

namespace {

// Measure steady-state gain at a given frequency by running a sine through and
// taking the peak after the transient has decayed.
double magnitudeAt(Biquad& filter, double freqHz, double sampleRate) {
    filter.reset();
    const int warmup = 8192, measure = 8192;
    std::vector<float> buf(256);
    float* ch[1] = { buf.data() };

    double phase = 0.0;
    const double inc = 2.0 * std::numbers::pi * freqHz / sampleRate;
    double peak = 0.0;

    for (int block = 0; block < (warmup + measure) / 256; ++block) {
        for (auto& s : buf) { s = static_cast<float>(std::sin(phase)); phase += inc; }
        AudioBufferView view(ch, 1, 256);
        filter.process(view);
        if (block * 256 >= warmup)
            for (auto s : buf) peak = std::max(peak, static_cast<double>(std::fabs(s)));
    }
    return peak;
}

} // namespace

TEST_CASE("low-pass shape", "[dsp]") {
    constexpr double sr = 48000.0;
    Biquad lp(Biquad::Type::LowPass, 1000.0, 0.7071);
    lp.prepare(sr, 256, 1);

    // Passband ~unity, cutoff ~-3 dB, stopband rolls off.
    CHECK_THAT(magnitudeAt(lp, 100.0, sr),  WithinAbs(1.0,   0.02));
    CHECK_THAT(magnitudeAt(lp, 1000.0, sr), WithinAbs(0.707, 0.03));
    CHECK(magnitudeAt(lp, 10000.0, sr) < 0.02);
}

TEST_CASE("high-pass shape", "[dsp]") {
    constexpr double sr = 48000.0;
    Biquad hp(Biquad::Type::HighPass, 1000.0, 0.7071);
    hp.prepare(sr, 256, 1);

    CHECK(magnitudeAt(hp, 50.0, sr) < 0.01);
    CHECK_THAT(magnitudeAt(hp, 1000.0, sr),  WithinAbs(0.707, 0.03));
    CHECK_THAT(magnitudeAt(hp, 12000.0, sr), WithinAbs(1.0,   0.05));
}

TEST_CASE("peak gain", "[dsp]") {
    constexpr double sr = 48000.0;
    Biquad peak(Biquad::Type::Peak, 1000.0, 1.0, 6.0);   // +6 dB = x1.995
    peak.prepare(sr, 256, 1);

    CHECK_THAT(magnitudeAt(peak, 1000.0, sr), WithinAbs(1.995, 0.05));
    CHECK_THAT(magnitudeAt(peak, 50.0, sr),   WithinAbs(1.0,   0.05));   // untouched far away
}

// A filter that is stable will decay to silence after the input stops. An
// unstable one blows up -- this catches coefficient sign errors immediately.
TEST_CASE("stable after an impulse", "[dsp]") {
    Biquad lp(Biquad::Type::LowPass, 200.0, 4.0);         // high Q, worst case
    lp.prepare(48000.0, 512, 1);

    std::vector<float> buf(512, 0.0f);
    buf[0] = 1.0f;
    float* ch[1] = { buf.data() };

    double tail = 0.0;
    for (int block = 0; block < 200; ++block) {
        AudioBufferView view(ch, 1, 512);
        lp.process(view);
        if (block > 100) for (auto s : buf) tail = std::max(tail, static_cast<double>(std::fabs(s)));
        std::fill(buf.begin(), buf.end(), 0.0f);
    }
    CHECK(tail < 1e-6);
}
