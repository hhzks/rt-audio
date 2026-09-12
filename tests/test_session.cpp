#include "ffi/Session.h"
#include "ffi/Utf8.h"
#include "dsp/Biquad.h"
#include "dsp/NoiseGate.h"
#include "dsp/Waveshaper.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cstring>
#include <limits>
#include <thread>

using namespace rt;
using Catch::Matchers::WithinAbs;

namespace {

DeviceConfig nullConfig() {
    DeviceConfig c;
    c.sampleRate  = 48000.0;
    c.blockFrames = 256;
    c.numChannels = 2;
    return c;
}

bool waitForCallbacks(Session& s, rt_snapshot& snap, std::uint64_t atLeast) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        s.snapshot(snap);
        if (snap.callbacks >= atLeast) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

std::uint64_t sum(const std::uint64_t (&counts)[RT_HIST_BUCKETS]) {
    std::uint64_t total = 0;
    for (auto c : counts) total += c;
    return total;
}

} // namespace

TEST_CASE("strip metadata before open", "[ffi]") {
    Session s;
    CHECK(s.stripCount() == 5);
    CHECK(std::strcmp(s.stripName(0), "Master") == 0);
    CHECK(std::strcmp(s.stripName(1), "High-pass") == 0);
    CHECK(std::strcmp(s.stripName(2), "Gate") == 0);
    CHECK(std::strcmp(s.stripName(3), "Drive") == 0);
    CHECK(std::strcmp(s.stripName(4), "Low shelf") == 0);
    CHECK(s.stripParams(0).size() == 3);
    CHECK(s.stripParams(1).size() == 2);
    CHECK(s.stripParams(2).size() == 3);
    CHECK(s.stripParams(3).size() == 3);
    CHECK(s.stripParams(4).size() == 3);
    CHECK(s.stripLatency(3) == 16);
    CHECK_THAT(s.paramDefault(4, Biquad::kGain), WithinAbs(2.0, 1e-12));
}

TEST_CASE("device queries before open are state errors", "[ffi]") {
    Session s;
    rt_snapshot snap{};
    CHECK_THROWS_AS(s.snapshot(snap), SessionStateError);
    CHECK_THROWS_AS(s.deviceStatus(), SessionStateError);
}

TEST_CASE("rejected values throw", "[ffi]") {
    Session s;
    CHECK_THROWS_AS(s.setParam(2, NoiseGate::kGainReduction, -10.0), SessionArgError);
    CHECK_THROWS_AS(s.setParam(1, Biquad::kGain, 3.0), SessionArgError);
    CHECK_THROWS_AS(s.setParam(9, 0, 0.0), SessionArgError);
    CHECK_THROWS_AS(s.setParam(3, Waveshaper::kDrive, std::numeric_limits<double>::quiet_NaN()),
                    SessionArgError);
}

TEST_CASE("callbacks advance and params round-trip", "[ffi]") {
    Session s;
    s.setParam(3, Waveshaper::kDrive, 100.0);   // clamps to 20
    s.setParam(0, 1, -3.0);                     // Master out gain
    s.open(Backend::Null, nullConfig());

    rt_snapshot snap{};
    CHECK(waitForCallbacks(s, snap, 5));
    CHECK(snap.running == 1);
    CHECK(snap.channels == 2);
    CHECK(snap.deadline_ns > 0);
    CHECK_THAT(snap.params[3][Waveshaper::kDrive], WithinAbs(20.0, 1e-12));
    CHECK_THAT(snap.params[0][1], WithinAbs(-3.0, 1e-12));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    s.snapshot(snap);
    CHECK(snap.in_peak[0] > 0.2f);   // NullDevice tone is 0.25
    CHECK(snap.in_peak[0] < 0.3f);
    CHECK(snap.out_peak[0] > 0.0f);
}

TEST_CASE("second open is a state error", "[ffi]") {
    Session s;
    s.open(Backend::Null, nullConfig());
    CHECK_THROWS_AS(s.open(Backend::Null, nullConfig()), SessionStateError);
}

TEST_CASE("histogram totals equal callbacks after stop", "[ffi]") {
    Session s;
    s.open(Backend::Null, nullConfig());
    rt_snapshot snap{};
    std::uint64_t drained = 0;
    for (int i = 0; i < 20; ++i) {
        s.snapshot(snap);
        drained += sum(snap.hist_window);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    s.stop();
    s.snapshot(snap);
    drained += sum(snap.hist_window);
    CHECK(snap.running == 0);
    CHECK(snap.callbacks > 0);
    CHECK(drained == snap.callbacks);
}

TEST_CASE("destroy while running", "[ffi]") {
    Session s;
    s.open(Backend::Null, nullConfig());
    rt_snapshot snap{};
    CHECK(waitForCallbacks(s, snap, 2));
}

TEST_CASE("UTF-8 truncation keeps whole code points", "[ffi]") {
    const char* src = "ab\xC2\xAE";   // "ab®": 4 bytes
    char buf[8];
    CHECK(copyUtf8Truncated(buf, 5, src) == 4);
    CHECK(std::strcmp(buf, src) == 0);
    CHECK(copyUtf8Truncated(buf, 4, src) == 2);   // would split ®
    CHECK(std::strcmp(buf, "ab") == 0);
    CHECK(copyUtf8Truncated(buf, 3, src) == 2);
    CHECK(copyUtf8Truncated(buf, 1, src) == 0);
    CHECK(buf[0] == '\0');
    CHECK(copyUtf8Truncated(buf, 0, src) == 0);
}

TEST_CASE("backend keys round-trip and resolve", "[session]") {
    for (Backend b : {Backend::Wasapi, Backend::Asio, Backend::Alsa, Backend::Null}) {
        const auto parsed = backendFromName(backendKey(b));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == b);
        CHECK(resolveBackend(b) == b);
    }
    CHECK(resolveBackend(Backend::Default) != Backend::Default);
#if defined(_WIN32)
    CHECK(resolveBackend(Backend::Default) == Backend::Wasapi);
#endif
}

TEST_CASE("device configs compare by value", "[session]") {
    const DeviceConfig a = nullConfig();
    DeviceConfig b = nullConfig();
    CHECK(a == b);
    b.outputId = "x";
    CHECK(!(a == b));
    b = a;
    b.exclusiveMode = true;
    CHECK(!(a == b));
}
