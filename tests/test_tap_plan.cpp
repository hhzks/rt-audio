#include "io/TapPlan.h"
#include <catch2/catch_test_macros.hpp>

using namespace rt;

namespace {
TapInputs inputs(Backend b, bool exclusive) {
    TapInputs in;
    in.backend   = b;
    in.exclusive = exclusive;
    return in;
}
} // namespace

TEST_CASE("wasapi on the same endpoint is the same device in both modes", "[io]") {
    CHECK(tapPlan(inputs(Backend::Wasapi, false), "spk", "spk") == TapAction::SameDevice);
    CHECK(tapPlan(inputs(Backend::Wasapi, true), "spk", "spk") == TapAction::SameDevice);
}

TEST_CASE("wasapi on another endpoint opens", "[io]") {
    CHECK(tapPlan(inputs(Backend::Wasapi, false), "spk", "phones") == TapAction::Open);
    CHECK(tapPlan(inputs(Backend::Wasapi, true), "spk", "phones") == TapAction::Open);
}

TEST_CASE("asio and null always open", "[io]") {
    CHECK(tapPlan(inputs(Backend::Asio, false), "spk", "spk") == TapAction::Open);
    CHECK(tapPlan(inputs(Backend::Null, false), "spk", "spk") == TapAction::Open);
}

TEST_CASE("an unresolved id never matches", "[io]") {
    CHECK(tapPlan(inputs(Backend::Wasapi, false), "", "") == TapAction::Open);
}
