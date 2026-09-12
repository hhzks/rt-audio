#include "ffi/rt_ffi.h"
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" std::size_t rt_tui_layout_probe(std::uint64_t* out, std::size_t cap);

#define LAYOUT(T)   v.push_back(sizeof(T)); v.push_back(alignof(T))
#define FIELD(T, f) v.push_back(offsetof(T, f))

namespace {

// Same order as rig_tui::ffi::layout_values().
std::vector<std::uint64_t> cLayout() {
    std::vector<std::uint64_t> v;
    LAYOUT(rt_open_config);
    FIELD(rt_open_config, backend);
    FIELD(rt_open_config, input_id);
    FIELD(rt_open_config, output_id);
    FIELD(rt_open_config, sample_rate);
    FIELD(rt_open_config, block_frames);
    FIELD(rt_open_config, exclusive);

    LAYOUT(rt_param_desc);
    FIELD(rt_param_desc, id);
    FIELD(rt_param_desc, name);
    FIELD(rt_param_desc, unit);
    FIELD(rt_param_desc, min);
    FIELD(rt_param_desc, max);
    FIELD(rt_param_desc, def);
    FIELD(rt_param_desc, taper);
    FIELD(rt_param_desc, flags);

    LAYOUT(rt_strip_desc);
    FIELD(rt_strip_desc, name);
    FIELD(rt_strip_desc, param_count);
    FIELD(rt_strip_desc, latency_frames);

    LAYOUT(rt_device_desc);
    FIELD(rt_device_desc, backend);
    FIELD(rt_device_desc, input);
    FIELD(rt_device_desc, output);
    FIELD(rt_device_desc, sample_rate);
    FIELD(rt_device_desc, claimed_rtt_ms);
    FIELD(rt_device_desc, block_frames);
    FIELD(rt_device_desc, channels);

    LAYOUT(rt_snapshot);
    FIELD(rt_snapshot, callbacks);
    FIELD(rt_snapshot, engine_xruns);
    FIELD(rt_snapshot, device_xruns);
    FIELD(rt_snapshot, capture_overruns);
    FIELD(rt_snapshot, capture_underruns);
    FIELD(rt_snapshot, in_clips);
    FIELD(rt_snapshot, out_clips);
    FIELD(rt_snapshot, deadline_ns);
    FIELD(rt_snapshot, hist_window);
    FIELD(rt_snapshot, in_peak);
    FIELD(rt_snapshot, out_peak);
    FIELD(rt_snapshot, params);
    FIELD(rt_snapshot, channels);
    FIELD(rt_snapshot, running);
    FIELD(rt_snapshot, device_error);

    LAYOUT(rt_device_info);
    FIELD(rt_device_info, id);
    FIELD(rt_device_info, name);
    FIELD(rt_device_info, max_input_channels);
    FIELD(rt_device_info, max_output_channels);
    FIELD(rt_device_info, default_sample_rate);
    FIELD(rt_device_info, is_default_input);
    FIELD(rt_device_info, is_default_output);

    LAYOUT(rt_config_desc);
    FIELD(rt_config_desc, backend);
    FIELD(rt_config_desc, input_id);
    FIELD(rt_config_desc, output_id);
    FIELD(rt_config_desc, sample_rate);
    FIELD(rt_config_desc, block_frames);
    FIELD(rt_config_desc, exclusive);

    LAYOUT(rt_latency_settings);
    FIELD(rt_latency_settings, repeats);
    FIELD(rt_latency_settings, amplitude);

    LAYOUT(rt_latency_repeat);
    FIELD(rt_latency_repeat, lag_ms);
    FIELD(rt_latency_repeat, correlation);
    FIELD(rt_latency_repeat, psr);
    FIELD(rt_latency_repeat, valid);
    FIELD(rt_latency_repeat, polarity_inverted);

    LAYOUT(rt_latency_status);
    FIELD(rt_latency_status, state);
    FIELD(rt_latency_status, kind);
    FIELD(rt_latency_status, phase);
    FIELD(rt_latency_status, repeat);
    FIELD(rt_latency_status, repeats);
    FIELD(rt_latency_status, latency_mode);
    FIELD(rt_latency_status, control_passed);
    FIELD(rt_latency_status, chain_valid);
    FIELD(rt_latency_status, clipped);
    FIELD(rt_latency_status, kept);
    FIELD(rt_latency_status, discarded);
    FIELD(rt_latency_status, measured_ms);
    FIELD(rt_latency_status, spread_ms);
    FIELD(rt_latency_status, computed_ms);
    FIELD(rt_latency_status, chain_measured_ms);
    FIELD(rt_latency_status, chain_reported_frames);
    FIELD(rt_latency_status, direct);
    FIELD(rt_latency_status, message);
    return v;
}

} // namespace

TEST_CASE("rust mirrors match the C header", "[ffi]") {
    const std::vector<std::uint64_t> expected = cLayout();
    std::vector<std::uint64_t> actual(128, 0);
    const std::size_t n = rt_tui_layout_probe(actual.data(), actual.size());
    REQUIRE(n == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        CHECK(actual[i] == expected[i]);
    }
}
