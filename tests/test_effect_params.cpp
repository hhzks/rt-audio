#include "dsp/Biquad.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

using namespace rt;
using Catch::Matchers::WithinAbs;

namespace {

double magnitudeAt(IEffect& fx, double freqHz, double sampleRate) {
    fx.reset();
    const int warmup = 8192, measure = 8192;
    std::vector<float> buf(256);
    float* ch[1] = { buf.data() };

    double phase = 0.0;
    const double inc = 2.0 * std::numbers::pi * freqHz / sampleRate;
    double peak = 0.0;

    for (int block = 0; block < (warmup + measure) / 256; ++block) {
        for (auto& s : buf) { s = static_cast<float>(std::sin(phase)); phase += inc; }
        AudioBufferView view(ch, 1, 256);
        fx.process(view);
        if (block * 256 >= warmup)
            for (auto s : buf) peak = std::max(peak, static_cast<double>(std::fabs(s)));
    }
    return peak;
}

bool isRow(const ParamInfo& p) { return (p.flags & (kReadOnly | kToggle)) == 0; }

void checkTable(const IEffect& fx) {
    const auto info = fx.params();
    for (std::size_t i = 0; i < info.size(); ++i) {
        CHECK(info[i].min <= fx.paramDefault(i));
        CHECK(fx.paramDefault(i) <= info[i].max);
        CHECK(info[i].min < info[i].max);
        if (isRow(info[i])) CHECK(std::strlen(info[i].name) <= 5);
        if (info[i].taper == Taper::Log) CHECK(info[i].min > 0.0);
        for (std::size_t j = i + 1; j < info.size(); ++j)
            CHECK(std::strcmp(info[i].id, info[j].id) != 0);
    }
}

} // namespace

TEST_CASE("biquad parameter count by type", "[dsp]") {
    CHECK(Biquad(Biquad::Type::LowPass,   1000.0, 0.707).params().size() == 2);
    CHECK(Biquad(Biquad::Type::HighPass,  1000.0, 0.707).params().size() == 2);
    CHECK(Biquad(Biquad::Type::Peak,      1000.0, 1.0, 3.0).params().size() == 3);
    CHECK(Biquad(Biquad::Type::LowShelf,  200.0,  0.707, 2.0).params().size() == 3);
    CHECK(Biquad(Biquad::Type::HighShelf, 5000.0, 0.707, -2.0).params().size() == 3);
}

TEST_CASE("biquad names by type", "[dsp]") {
    CHECK(std::strcmp(Biquad(Biquad::Type::LowPass,   1000.0, 0.707).name(), "Low-pass") == 0);
    CHECK(std::strcmp(Biquad(Biquad::Type::HighPass,  1000.0, 0.707).name(), "High-pass") == 0);
    CHECK(std::strcmp(Biquad(Biquad::Type::Peak,      1000.0, 1.0).name(),   "Peak") == 0);
    CHECK(std::strcmp(Biquad(Biquad::Type::LowShelf,  200.0,  0.707).name(), "Low shelf") == 0);
    CHECK(std::strcmp(Biquad(Biquad::Type::HighShelf, 5000.0, 0.707).name(), "High shelf") == 0);
}

TEST_CASE("biquad defaults are the constructor values", "[dsp]") {
    Biquad shelf(Biquad::Type::LowShelf, 200.0, 0.707, 2.0);
    CHECK_THAT(shelf.paramDefault(Biquad::kFreq), WithinAbs(200.0, 1e-12));
    CHECK_THAT(shelf.paramDefault(Biquad::kQ),    WithinAbs(0.707, 1e-12));
    CHECK_THAT(shelf.paramDefault(Biquad::kGain), WithinAbs(2.0,   1e-12));
    CHECK_THAT(shelf.getParam(Biquad::kFreq),     WithinAbs(200.0, 1e-12));
    checkTable(shelf);
    checkTable(Biquad(Biquad::Type::HighPass, 80.0, 0.707));
}

TEST_CASE("biquad rejects gain on a high-pass", "[dsp]") {
    Biquad hp(Biquad::Type::HighPass, 80.0, 0.707);
    CHECK(!hp.setParam(Biquad::kGain, 3.0));
}

TEST_CASE("biquad clamps frequency", "[dsp]") {
    Biquad hp(Biquad::Type::HighPass, 80.0, 0.707);
    CHECK(hp.setParam(Biquad::kFreq, 1.0e6));
    CHECK_THAT(hp.getParam(Biquad::kFreq), WithinAbs(20000.0, 1e-9));
}

TEST_CASE("changed biquad converges to a fresh filter", "[dsp]") {
    constexpr double sr = 48000.0;
    Biquad changed(Biquad::Type::HighPass, 80.0, 0.707);
    changed.prepare(sr, 256, 1);
    CHECK(changed.setParam(Biquad::kFreq, 1000.0));

    Biquad fresh(Biquad::Type::HighPass, 1000.0, 0.707);
    fresh.prepare(sr, 256, 1);

    const double a = magnitudeAt(changed, 1000.0, sr);
    const double b = magnitudeAt(fresh, 1000.0, sr);
    CHECK_THAT(a, WithinAbs(b, 1e-6));
    CHECK_THAT(a, WithinAbs(0.707, 0.03));
}
