#include "ffi/rt_ffi.h"
#include "ffi/DeviceInfoCopy.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
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

TEST_CASE("enumerate reports the total and fills up to cap", "[ffi]") {
    rt_session* s = rt_session_create();
    int32_t total = -1;
    rt_device_info info[4]{};
    CHECK(rt_session_enumerate(s, info, 4, &total) == RT_E_STATE);

    rt_open_config cfg = nullConfig();
    REQUIRE(rt_session_open(s, &cfg) == RT_OK);
    CHECK(rt_session_enumerate(s, nullptr, 0, &total) == RT_OK);
    CHECK(total == 1);
    CHECK(rt_session_enumerate(s, info, 4, &total) == RT_OK);
    CHECK(total == 1);
    CHECK(std::strcmp(info[0].id, "null") == 0);
    CHECK(info[0].max_input_channels == 2);
    CHECK(info[0].is_default_output == 1);

    CHECK(rt_session_enumerate(s, info, 4, nullptr) == RT_E_ARG);
    CHECK(rt_session_enumerate(s, nullptr, 1, &total) == RT_E_ARG);
    CHECK(rt_session_enumerate(s, info, -1, &total) == RT_E_ARG);
    CHECK(rt_session_enumerate(nullptr, info, 4, &total) == RT_E_ARG);
    rt_session_destroy(s);
}

TEST_CASE("config reports the requested values", "[ffi]") {
    rt_session* s = rt_session_create();
    rt_config_desc c{};
    CHECK(rt_session_config(s, &c) == RT_E_STATE);

    rt_open_config cfg = nullConfig();
    REQUIRE(rt_session_open(s, &cfg) == RT_OK);
    CHECK(rt_session_config(s, nullptr) == RT_E_ARG);
    CHECK(rt_session_config(s, &c) == RT_OK);
    CHECK(std::strcmp(c.backend, "null") == 0);
    CHECK(c.input_id[0] == '\0');
    CHECK(c.output_id[0] == '\0');
    CHECK_THAT(c.sample_rate, WithinAbs(48000.0, 1e-9));
    CHECK(c.block_frames == 256);
    CHECK(c.exclusive == 0);
    rt_session_destroy(s);
}

TEST_CASE("reconfigure over the C ABI", "[ffi]") {
    rt_session* s = rt_session_create();
    rt_open_config cfg = nullConfig();
    int32_t outcome = -1;
    CHECK(rt_session_reconfigure(s, &cfg, &outcome) == RT_E_STATE);
    REQUIRE(rt_session_open(s, &cfg) == RT_OK);

    cfg.block_frames = 512;
    CHECK(rt_session_reconfigure(s, &cfg, nullptr) == RT_E_ARG);
    CHECK(rt_session_reconfigure(s, nullptr, &outcome) == RT_E_ARG);
    CHECK(rt_session_reconfigure(nullptr, &cfg, &outcome) == RT_E_ARG);
    CHECK(rt_session_reconfigure(s, &cfg, &outcome) == RT_OK);
    CHECK(outcome == RT_RECONF_APPLIED);
    rt_device_desc dev{};
    CHECK(rt_session_device(s, &dev) == RT_OK);
    CHECK(dev.block_frames == 512);

    cfg.backend = nullptr;
    cfg.block_frames = 128;
    CHECK(rt_session_reconfigure(s, &cfg, &outcome) == RT_OK);

#if defined(_WIN32)
    cfg.backend = "wasapi";
#else
    cfg.backend = "alsa";
#endif
    char buf[128];
    CHECK(rt_session_reconfigure(s, &cfg, &outcome) == RT_E_ARG);
    CHECK(rt_session_last_error(s, buf, sizeof buf) > 0);
    CHECK(std::string(buf).find("backend") != std::string::npos);
    cfg.backend = "nosuch";
    CHECK(rt_session_reconfigure(s, &cfg, &outcome) == RT_E_ARG);
    rt_session_destroy(s);
}

TEST_CASE("the driver panel call checks its arguments and state", "[ffi]") {
    int32_t result = -1;
    CHECK(rt_session_control_panel(nullptr, &result) == RT_E_ARG);
    rt_session* s = rt_session_create();
    CHECK(rt_session_control_panel(s, nullptr) == RT_E_ARG);
    CHECK(rt_session_control_panel(s, &result) == RT_E_STATE);
    rt_open_config cfg = nullConfig();
    REQUIRE(rt_session_open(s, &cfg) == RT_OK);
    CHECK(rt_session_control_panel(s, &result) == RT_E_STATE);
    char buf[128];
    rt_session_last_error(s, buf, sizeof buf);
    CHECK(std::string(buf) == "this backend has no driver panel");
    auto snap = std::make_unique<rt_snapshot>();
    REQUIRE(rt_session_snapshot(s, snap.get()) == RT_OK);
    CHECK(snap->panel_open == 0);
    rt_session_destroy(s);
}

TEST_CASE("device ids that do not fit are rejected", "[ffi]") {
    rt_session* s = rt_session_create();
    const std::string longId(RT_ID_BYTES, 'x');
    rt_open_config cfg = nullConfig();
    cfg.input_id = longId.c_str();
    CHECK(rt_session_open(s, &cfg) == RT_E_ARG);

    cfg.input_id = nullptr;
    REQUIRE(rt_session_open(s, &cfg) == RT_OK);
    int32_t outcome = -1;
    cfg.output_id = longId.c_str();
    CHECK(rt_session_reconfigure(s, &cfg, &outcome) == RT_E_ARG);

    const std::string maxId(RT_ID_BYTES - 1, 'y');
    cfg.output_id = maxId.c_str();
    CHECK(rt_session_reconfigure(s, &cfg, &outcome) == RT_OK);   // the Null device ignores ids
    rt_config_desc c{};
    CHECK(rt_session_config(s, &c) == RT_OK);
    CHECK(std::string(c.output_id) == maxId);
    rt_session_destroy(s);
}

TEST_CASE("the ring margin crosses the C ABI", "[ffi]") {
    rt_session* s = rt_session_create();
    rt_open_config cfg = nullConfig();
    cfg.ring_blocks = 2.5;
    CHECK(rt_session_open(s, &cfg) == RT_E_ARG);
    char buf[128];
    CHECK(rt_session_last_error(s, buf, sizeof buf) > 0);
    CHECK(std::string(buf) == "ring margin must be 1.0 to 2.0 blocks, got 2.5");
    cfg.ring_blocks = std::numeric_limits<double>::quiet_NaN();
    CHECK(rt_session_open(s, &cfg) == RT_E_ARG);

    cfg.ring_blocks = 0.0;
    REQUIRE(rt_session_open(s, &cfg) == RT_OK);
    rt_config_desc c{};
    REQUIRE(rt_session_config(s, &c) == RT_OK);
    CHECK_THAT(c.ring_blocks, WithinAbs(2.0, 1e-12));

    int32_t outcome = -1;
    cfg.ring_blocks = 1.5;
    CHECK(rt_session_reconfigure(s, &cfg, &outcome) == RT_OK);
    CHECK(outcome == RT_RECONF_APPLIED);
    REQUIRE(rt_session_config(s, &c) == RT_OK);
    CHECK_THAT(c.ring_blocks, WithinAbs(1.5, 1e-12));

    cfg.ring_blocks = 0.5;
    CHECK(rt_session_reconfigure(s, &cfg, &outcome) == RT_E_ARG);
    REQUIRE(rt_session_config(s, &c) == RT_OK);
    CHECK_THAT(c.ring_blocks, WithinAbs(1.5, 1e-12));
    rt_session_destroy(s);
}

TEST_CASE("device info copy drops ids that do not fit", "[ffi]") {
    rt::DeviceInfo d;
    d.id   = std::string(RT_ID_BYTES - 1, 'a');
    d.name = std::string(RT_NAME_BYTES - 2, 'n') + "\xC2\xAE";   // the (R) sign straddles the limit
    d.maxInputChannels = 2;
    d.isDefaultInput   = true;

    rt_device_info out{};
    REQUIRE(rt::toDeviceInfo(d, out));
    CHECK(std::strlen(out.id) == static_cast<std::size_t>(RT_ID_BYTES - 1));
    CHECK(std::strlen(out.name) == static_cast<std::size_t>(RT_NAME_BYTES - 2));
    CHECK(out.max_input_channels == 2);
    CHECK(out.is_default_input == 1);
    CHECK(out.is_default_output == 0);

    d.id.push_back('a');
    CHECK(!rt::toDeviceInfo(d, out));
}

TEST_CASE("latency calls check their arguments and state", "[ffi]") {
    rt_latency_settings settings{5, 0.5f};
    rt_latency_status st{};
    CHECK(rt_session_latency_enter(nullptr) == RT_E_ARG);
    CHECK(rt_session_latency_leave(nullptr) == RT_E_ARG);
    CHECK(rt_session_latency_start(nullptr, RT_LAT_CONTROL, &settings) == RT_E_ARG);
    CHECK(rt_session_latency_cancel(nullptr) == RT_E_ARG);
    CHECK(rt_session_latency_status(nullptr, &st) == RT_E_ARG);

    rt_session* s = rt_session_create();
    CHECK(rt_session_latency_start(s, RT_LAT_CONTROL, nullptr) == RT_E_ARG);
    CHECK(rt_session_latency_status(s, nullptr) == RT_E_ARG);
    CHECK(rt_session_latency_enter(s) == RT_E_STATE);
    CHECK(rt_session_latency_leave(s) == RT_E_STATE);
    CHECK(rt_session_latency_start(s, RT_LAT_CONTROL, &settings) == RT_E_STATE);
    CHECK(rt_session_latency_cancel(s) == RT_E_STATE);
    CHECK(rt_session_latency_status(s, &st) == RT_E_STATE);

    rt_open_config cfg = nullConfig();
    REQUIRE(rt_session_open(s, &cfg) == RT_OK);
    CHECK(rt_session_latency_start(s, RT_LAT_CONTROL, &settings) == RT_E_STATE);   // not in latency mode
    CHECK(rt_session_latency_enter(s) == RT_OK);
    CHECK(rt_session_latency_status(s, &st) == RT_OK);
    CHECK(st.latency_mode == 1);
    CHECK(st.state == RT_LAT_IDLE);
    CHECK(st.control_passed == 0);

    CHECK(rt_session_latency_start(s, 7, &settings) == RT_E_ARG);
    rt_latency_settings zero{0, 0.5f};
    CHECK(rt_session_latency_start(s, RT_LAT_CONTROL, &zero) == RT_E_ARG);
    CHECK(rt_session_latency_start(s, RT_LAT_MEASURE, &settings) == RT_E_STATE);   // no control yet
    char buf[128];
    CHECK(rt_session_last_error(s, buf, sizeof buf) > 0);
    CHECK(std::string(buf).find("negative control") != std::string::npos);

    CHECK(rt_session_latency_cancel(s) == RT_OK);
    CHECK(rt_session_latency_leave(s) == RT_OK);
    CHECK(rt_session_latency_status(s, &st) == RT_OK);
    CHECK(st.latency_mode == 0);
    rt_session_destroy(s);
}
