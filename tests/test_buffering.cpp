#include "io/Buffering.h"
#include "io/IAudioDevice.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <limits>

using namespace rt;
using Catch::Matchers::WithinAbs;

TEST_CASE("ring target is the margin times the block, rounded", "[io]") {
    CHECK(ringTargetFrames(144, 2.0) == 288u);
    CHECK(ringTargetFrames(144, 1.75) == 252u);
    CHECK(ringTargetFrames(144, 1.5) == 216u);
    CHECK(ringTargetFrames(144, 1.25) == 180u);
    CHECK(ringTargetFrames(144, 1.0) == 144u);
    CHECK(ringTargetFrames(1056, 2.0) == 2112u);
    CHECK(ringTargetFrames(145, 1.25) == 181u);
    CHECK(ringTargetFrames(1057, 1.5) == 1586u);
}

TEST_CASE("the buffered round trip adds the ring to the endpoint buffers", "[io]") {
    CHECK_THAT(bufferedRoundTripMs(144, 144, 288, 48000, 16, 48000), WithinAbs(12.333, 0.001));
    CHECK_THAT(bufferedRoundTripMs(144, 144, 144, 48000, 16, 48000), WithinAbs(9.333, 0.001));
    CHECK_THAT(bufferedRoundTripMs(441, 480, 960, 48000, 16, 44100), WithinAbs(39.550, 0.001));
}

TEST_CASE("the ring margin range is 1 to 2 blocks", "[io]") {
    CHECK(validRingBlocks(1.0));
    CHECK(validRingBlocks(1.3));
    CHECK(validRingBlocks(2.0));
    CHECK_FALSE(validRingBlocks(0.0));
    CHECK_FALSE(validRingBlocks(0.99));
    CHECK_FALSE(validRingBlocks(2.01));
    CHECK_FALSE(validRingBlocks(std::numeric_limits<double>::quiet_NaN()));
    CHECK(DeviceConfig{}.ringBlocks == kDefaultRingBlocks);
}
