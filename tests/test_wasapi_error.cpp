#include "io/wasapi/WasapiError.h"
#include <catch2/catch_test_macros.hpp>

using namespace rt;

TEST_CASE("success codes are not errors", "[io]") {
    CHECK(wasapiActionFor(0) == WasapiAction::None);   // S_OK
    CHECK(wasapiActionFor(1) == WasapiAction::None);   // S_FALSE
}

TEST_CASE("lost-device codes fail", "[io]") {
    CHECK(wasapiActionFor(kHrDeviceInvalidated) == WasapiAction::Fail);
    CHECK(wasapiActionFor(kHrServiceNotRunning) == WasapiAction::Fail);
}

TEST_CASE("other failures keep today's behaviour", "[io]") {
    CHECK(wasapiActionFor(static_cast<std::int32_t>(0x80004005u)) == WasapiAction::None); // E_FAIL
    CHECK(wasapiActionFor(static_cast<std::int32_t>(0x88890001u)) == WasapiAction::None); // not initialized
}
