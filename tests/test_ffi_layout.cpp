#include "ffi/rt_ffi.h"
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" std::size_t rt_tui_layout_probe(std::uint64_t* out, std::size_t cap);
extern "C" std::int32_t rt_tui_panel_probe(std::int32_t code);

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
    FIELD(rt_open_config, ring_blocks);

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
    FIELD(rt_snapshot, panel_open);
    FIELD(rt_snapshot, device_error);
    FIELD(rt_snapshot, system_peak);
    FIELD(rt_snapshot, system_state);
    FIELD(rt_snapshot, system_text);

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
    FIELD(rt_config_desc, ring_blocks);
    FIELD(rt_config_desc, system_source);

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

    LAYOUT(rt_backend_caps);
    FIELD(rt_backend_caps, flags);
    FIELD(rt_backend_caps, display_name);
    FIELD(rt_backend_caps, notice);

    for (int c : {RT_PANEL_OPENED, RT_PANEL_MODAL, RT_PANEL_ALREADY_OPEN, RT_PANEL_NONE})
        v.push_back(static_cast<std::uint64_t>(c));
    for (std::uint64_t c : {RT_CAP_RING, RT_CAP_ONE_DRIVER, RT_CAP_DRIVER_PANEL, RT_CAP_EXCLUSIVE_MODE,
                            RT_CAP_RATE_FROM_DEVICE, RT_CAP_BLOCK_ZERO_PREFERRED, RT_CAP_BLOCK_ROUNDED,
                            RT_CAP_SYSTEM_AUDIO})
        v.push_back(c);
    for (int c : {RT_SYS_OFF, RT_SYS_IDLE, RT_SYS_PLAYING, RT_SYS_SAME_DEVICE, RT_SYS_ERROR})
        v.push_back(static_cast<std::uint64_t>(c));
    return v;
}

} // namespace

TEST_CASE("rust mirrors match the C header", "[ffi]") {
    const std::vector<std::uint64_t> expected = cLayout();
    std::vector<std::uint64_t> actual(160, 0);
    const std::size_t n = rt_tui_layout_probe(actual.data(), actual.size());
    REQUIRE(n == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        CHECK(actual[i] == expected[i]);
    }
}

// The probe answers 0..3 for Opened, Modal, AlreadyOpen, NoDriverPanel and -1 for an error.
TEST_CASE("rust maps each panel result code to its outcome", "[ffi]") {
    CHECK(rt_tui_panel_probe(RT_PANEL_OPENED) == 0);
    CHECK(rt_tui_panel_probe(RT_PANEL_MODAL) == 1);
    CHECK(rt_tui_panel_probe(RT_PANEL_ALREADY_OPEN) == 2);
    CHECK(rt_tui_panel_probe(RT_PANEL_NONE) == 3);
    CHECK(rt_tui_panel_probe(99) == -1);
}
