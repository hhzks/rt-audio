#include "io/DeviceFactory.h"
#include <catch2/catch_test_macros.hpp>

using namespace rt;

TEST_CASE("each backend reports its rules", "[io]") {
    const BackendCaps w = backendCaps(Backend::Wasapi);
    CHECK(w.ring);
    CHECK(!w.oneDriver);
    CHECK(!w.driverPanel);
    CHECK(w.exclusiveMode);
    CHECK(w.rateFromDevice);
    CHECK(!w.blockZeroPreferred);
    CHECK(w.blockRounded);
    CHECK(w.displayName == "WASAPI");
    CHECK(w.notice.empty());

    const BackendCaps a = backendCaps(Backend::Asio);
    CHECK(!a.ring);
    CHECK(a.oneDriver);
    CHECK(a.driverPanel);
    CHECK(!a.exclusiveMode);
    CHECK(!a.rateFromDevice);
    CHECK(a.blockZeroPreferred);
    CHECK(a.blockRounded);
    CHECK(a.displayName == "ASIO\xC2\xAE");
    CHECK(a.notice == kAsioTrademarkNotice);

    const BackendCaps l = backendCaps(Backend::Alsa);
    CHECK(l.ring);
    CHECK(!l.oneDriver);
    CHECK(!l.driverPanel);
    CHECK(!l.exclusiveMode);
    CHECK(!l.rateFromDevice);
    CHECK(!l.blockZeroPreferred);
    CHECK(!l.blockRounded);
    CHECK(l.displayName == "ALSA");

    const BackendCaps n = backendCaps(Backend::Null);
    CHECK(!n.ring);
    CHECK(!n.oneDriver);
    CHECK(!n.driverPanel);
    CHECK(!n.exclusiveMode);
    CHECK(!n.rateFromDevice);
    CHECK(!n.blockZeroPreferred);
    CHECK(!n.blockRounded);
    CHECK(n.displayName == "NULL");
    CHECK(n.notice.empty());
}

TEST_CASE("the default backend reports the platform backend", "[io]") {
    CHECK(backendCaps(Backend::Default).displayName ==
          backendCaps(resolveBackend(Backend::Default)).displayName);
}

TEST_CASE("system audio follows the platform, never alsa", "[io]") {
#ifdef _WIN32
    CHECK(backendCaps(Backend::Wasapi).systemAudio);
    CHECK(backendCaps(Backend::Asio).systemAudio);
    CHECK(backendCaps(Backend::Null).systemAudio);
#else
    CHECK(!backendCaps(Backend::Null).systemAudio);
#endif
    CHECK(!backendCaps(Backend::Alsa).systemAudio);
}
