#include "core/ParamInfo.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <limits>

using namespace rt;
using Catch::Matchers::WithinAbs;

namespace {

constexpr std::array<ParamInfo, 3> kInfo{{
    {"level", "lvl", "dB", -60.0, 12.0, -6.0, Taper::Linear, 0},
    {"on",    "on",  "",    0.0,   1.0,  1.0, Taper::Linear, kToggle},
    {"gr",    "gr",  "dB", -26.0,  0.0,  0.0, Taper::Linear, kReadOnly},
}};

} // namespace

TEST_CASE("defaults load", "[core]") {
    ParamBlock<3> p(kInfo);
    CHECK_THAT(p.get(0), WithinAbs(-6.0, 1e-12));
    CHECK_THAT(p.defaultValue(0), WithinAbs(-6.0, 1e-12));
    CHECK_THAT(p.get(1), WithinAbs(1.0, 1e-12));
    CHECK(p.info().size() == 3);
}

TEST_CASE("set clamps to the range", "[core]") {
    ParamBlock<3> p(kInfo);
    CHECK(p.set(0, 100.0));
    CHECK_THAT(p.get(0), WithinAbs(12.0, 1e-12));
    CHECK(p.set(0, -100.0));
    CHECK_THAT(p.get(0), WithinAbs(-60.0, 1e-12));
}

TEST_CASE("non-finite values are rejected", "[core]") {
    ParamBlock<3> p(kInfo);
    CHECK(p.set(0, -3.0));
    CHECK(!p.set(0, std::numeric_limits<double>::quiet_NaN()));
    CHECK(!p.set(0, std::numeric_limits<double>::infinity()));
    CHECK(!p.set(0, -std::numeric_limits<double>::infinity()));
    CHECK_THAT(p.get(0), WithinAbs(-3.0, 1e-12));
}

TEST_CASE("read-only refuses set but accepts publish", "[core]") {
    ParamBlock<3> p(kInfo);
    CHECK(!p.set(2, -10.0));
    CHECK_THAT(p.get(2), WithinAbs(0.0, 1e-12));
    p.publish(2, -10.0);
    CHECK_THAT(p.get(2), WithinAbs(-10.0, 1e-12));
}

TEST_CASE("toggle snaps to 0 or 1", "[core]") {
    ParamBlock<3> p(kInfo);
    CHECK(p.set(1, 0.2));
    CHECK_THAT(p.get(1), WithinAbs(0.0, 1e-12));
    CHECK(p.set(1, 0.7));
    CHECK_THAT(p.get(1), WithinAbs(1.0, 1e-12));
    CHECK(p.set(1, 0.5));
    CHECK_THAT(p.get(1), WithinAbs(1.0, 1e-12));
}

TEST_CASE("setDefault sets value and default", "[core]") {
    ParamBlock<3> p(kInfo);
    p.setDefault(0, 2.0);
    CHECK_THAT(p.get(0), WithinAbs(2.0, 1e-12));
    CHECK_THAT(p.defaultValue(0), WithinAbs(2.0, 1e-12));
    CHECK(p.set(0, 5.0));
    CHECK_THAT(p.defaultValue(0), WithinAbs(2.0, 1e-12));
}

TEST_CASE("out-of-range index is rejected", "[core]") {
    ParamBlock<3> p(kInfo);
    CHECK(!p.set(3, 0.0));
}
