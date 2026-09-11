#include "io/alsa/AlsaError.h"
#include <catch2/catch_test_macros.hpp>

using namespace rt;

TEST_CASE("success is not an error", "[io]") {
    CHECK(alsaActionFor(0) == AlsaAction::None);
    CHECK(alsaActionFor(1200) == AlsaAction::None);
}

TEST_CASE("transient codes retry", "[io]") {
    CHECK(alsaActionFor(-EINTR)  == AlsaAction::Retry);
    CHECK(alsaActionFor(-EAGAIN) == AlsaAction::Retry);
}

TEST_CASE("xrun and suspend recover", "[io]") {
    CHECK(alsaActionFor(-EPIPE)    == AlsaAction::Recover);
    CHECK(alsaActionFor(-ESTRPIPE) == AlsaAction::Recover);
}

TEST_CASE("unmapped codes fail", "[io]") {
    CHECK(alsaActionFor(-ENODEV) == AlsaAction::Fail);
    CHECK(alsaActionFor(-12345)  == AlsaAction::Fail);
}
