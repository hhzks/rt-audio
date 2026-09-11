#include "ffi/rt_ffi.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cstring>
#include <limits>
#include <thread>

using Catch::Matchers::WithinAbs;

namespace {

rt_open_config nullConfig() {
    rt_open_config c{};
    c.backend      = "null";
    c.sample_rate  = 48000.0;
    c.block_frames = 256;
    return c;
}

} // namespace

TEST_CASE("null handles are rejected", "[ffi]") {
    rt_snapshot    snap{};
    rt_device_desc dev{};
    rt_strip_desc  strip{};
    rt_param_desc  param{};
    rt_open_config cfg = nullConfig();
    CHECK(rt_session_open(nullptr, &cfg) == RT_E_ARG);
    CHECK(rt_session_stop(nullptr) == RT_E_ARG);
    CHECK(rt_session_device(nullptr, &dev) == RT_E_ARG);
    CHECK(rt_session_strip_count(nullptr) == -1);
    CHECK(rt_session_strip(nullptr, 0, &strip) == RT_E_ARG);
    CHECK(rt_session_param(nullptr, 0, 0, &param) == RT_E_ARG);
    CHECK(rt_session_set_param(nullptr, 0, 0, 0.0) == RT_E_ARG);
    CHECK(rt_session_snapshot(nullptr, &snap) == RT_E_ARG);
    CHECK(rt_session_last_error(nullptr, nullptr, 0) == 0);
    rt_session_destroy(nullptr);
}

TEST_CASE("null output pointers are rejected", "[ffi]") {
    rt_session* s = rt_session_create();
    REQUIRE(s != nullptr);
    CHECK(rt_session_open(s, nullptr) == RT_E_ARG);
    CHECK(rt_session_strip(s, 0, nullptr) == RT_E_ARG);
    CHECK(rt_session_param(s, 0, 0, nullptr) == RT_E_ARG);
    CHECK(rt_session_snapshot(s, nullptr) == RT_E_ARG);
    CHECK(rt_session_device(s, nullptr) == RT_E_ARG);
    rt_session_destroy(s);
}

TEST_CASE("metadata and index checks", "[ffi]") {
    rt_session* s = rt_session_create();
    CHECK(rt_session_strip_count(s) == 5);

    rt_strip_desc strip{};
    CHECK(rt_session_strip(s, 3, &strip) == RT_OK);
    CHECK(std::strcmp(strip.name, "Drive") == 0);
    CHECK(strip.param_count == 3);
    CHECK(strip.latency_frames == 16);
    CHECK(rt_session_strip(s, 5, &strip) == RT_E_ARG);
    CHECK(rt_session_strip(s, -1, &strip) == RT_E_ARG);

    rt_param_desc p{};
    CHECK(rt_session_param(s, 2, 2, &p) == RT_OK);
    CHECK(std::strcmp(p.id, "gain_reduction") == 0);
    CHECK((p.flags & RT_FLAG_READ_ONLY) != 0);
    CHECK(rt_session_param(s, 4, 2, &p) == RT_OK);
    CHECK_THAT(p.def, WithinAbs(2.0, 1e-12));
    CHECK(p.taper == RT_TAPER_LINEAR);
    CHECK(rt_session_param(s, 1, 0, &p) == RT_OK);
    CHECK(p.taper == RT_TAPER_LOG);
    CHECK(rt_session_param(s, 1, 2, &p) == RT_E_ARG);
    rt_session_destroy(s);
}

TEST_CASE("a rejected set is an argument error with a message", "[ffi]") {
    rt_session* s = rt_session_create();
    CHECK(rt_session_set_param(s, 2, 2, -10.0) == RT_E_ARG);
    CHECK(rt_session_set_param(s, 3, 1, std::numeric_limits<double>::quiet_NaN()) == RT_E_ARG);

    char buf[64];
    const size_t n = rt_session_last_error(s, buf, sizeof buf);
    CHECK(n > 0);
    CHECK(std::strlen(buf) == n);
    char tiny[4];
    CHECK(rt_session_last_error(s, tiny, sizeof tiny) == n);
    CHECK(std::strlen(tiny) == 3);
    rt_session_destroy(s);
}

TEST_CASE("device queries before open are state errors", "[ffi]") {
    rt_session* s = rt_session_create();
    rt_snapshot    snap{};
    rt_device_desc dev{};
    CHECK(rt_session_snapshot(s, &snap) == RT_E_STATE);
    CHECK(rt_session_device(s, &dev) == RT_E_STATE);
    rt_session_destroy(s);
}

TEST_CASE("unknown and unavailable backends", "[ffi]") {
    rt_session* s = rt_session_create();
    rt_open_config cfg = nullConfig();
    char buf[128];

    cfg.backend = "nosuch";
    CHECK(rt_session_open(s, &cfg) == RT_E_ARG);
    CHECK(rt_session_last_error(s, buf, sizeof buf) > 0);

#if defined(_WIN32)
    cfg.backend = "alsa";
#else
    cfg.backend = "wasapi";
#endif
    CHECK(rt_session_open(s, &cfg) == RT_E_DEVICE);
    CHECK(rt_session_last_error(s, buf, sizeof buf) > 0);
    rt_session_destroy(s);
}

TEST_CASE("open, snapshot, stop", "[ffi]") {
    rt_session* s = rt_session_create();
    rt_open_config cfg = nullConfig();
    CHECK(rt_session_open(s, &cfg) == RT_OK);
    CHECK(rt_session_open(s, &cfg) == RT_E_STATE);

    rt_device_desc dev{};
    CHECK(rt_session_device(s, &dev) == RT_OK);
    CHECK(std::strcmp(dev.backend, "Null") == 0);
    CHECK(dev.block_frames == 256);
    CHECK(dev.channels == 2);

    rt_snapshot snap{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (snap.callbacks < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (rt_session_snapshot(s, &snap) != RT_OK) break;
    }
    CHECK(snap.callbacks >= 3);
    CHECK(snap.running == 1);

    CHECK(rt_session_stop(s) == RT_OK);
    CHECK(rt_session_snapshot(s, &snap) == RT_OK);
    CHECK(snap.running == 0);
    rt_session_destroy(s);
}

TEST_CASE("histogram functions match the engine", "[ffi]") {
    uint64_t counts[RT_HIST_BUCKETS] = {};
    counts[40] = 99;
    counts[80] = 1;
    CHECK(rt_hist_percentile_ns(counts, 0.5) == rt_hist_bucket_upper_ns(40));
    CHECK(rt_hist_percentile_ns(counts, 1.0) == rt_hist_bucket_upper_ns(80));
    CHECK(rt_hist_percentile_ns(nullptr, 0.5) == 0);
    uint64_t empty[RT_HIST_BUCKETS] = {};
    CHECK(rt_hist_percentile_ns(empty, 0.99) == 0);
}
