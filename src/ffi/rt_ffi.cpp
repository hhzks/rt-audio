#include "ffi/rt_ffi.h"
#include "ffi/DeviceInfoCopy.h"
#include "ffi/Session.h"
#include "ffi/Utf8.h"
#include "engine/RtHistogram.h"

#include <exception>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

static_assert(RT_RECONF_APPLIED == static_cast<int>(rt::ReconfigureResult::Applied));
static_assert(RT_RECONF_ROLLED_BACK == static_cast<int>(rt::ReconfigureResult::RolledBack));
static_assert(RT_RECONF_STOPPED == static_cast<int>(rt::ReconfigureResult::Stopped));
static_assert(RT_LAT_CONTROL == static_cast<int>(rt::LatencyKind::Control));
static_assert(RT_LAT_MEASURE == static_cast<int>(rt::LatencyKind::Measure));

struct rt_session {
    rt::Session         session;
    mutable std::string lastError;
};

namespace {

template <class S, class F>
int32_t guarded(S* s, F&& f) {
    if (s == nullptr) return RT_E_ARG;
    try {
        f(s->session);
        return RT_OK;
    } catch (const rt::SessionArgError& e) {
        s->lastError = e.what();
        return RT_E_ARG;
    } catch (const rt::SessionStateError& e) {
        s->lastError = e.what();
        return RT_E_STATE;
    } catch (const std::exception& e) {
        s->lastError = e.what();
        return RT_E_INTERNAL;
    } catch (...) {
        s->lastError = "unknown exception";
        return RT_E_INTERNAL;
    }
}

// Sets lastError and returns nullopt for an unknown name. NULL means Default.
std::optional<rt::Backend> parseBackend(rt_session* s, const char* name) {
    if (name == nullptr) return rt::Backend::Default;
    const auto b = rt::backendFromName(name);
    if (!b) s->lastError = std::string("unknown backend: ") + name;
    return b;
}

// Returns RT_OK, or RT_E_ARG with lastError set when an id cannot round-trip or the ring
// margin is out of range.
int32_t toDeviceConfig(rt_session* s, const rt_open_config& cfg, rt::DeviceConfig& dc) {
    for (const char* id : {cfg.input_id, cfg.output_id}) {
        if (id != nullptr && std::string_view(id).size() >= RT_ID_BYTES) {
            s->lastError = "device id longer than " + std::to_string(RT_ID_BYTES - 1) + " bytes";
            return RT_E_ARG;
        }
    }
    dc = rt::DeviceConfig{};
    if (cfg.input_id != nullptr)  dc.inputId  = cfg.input_id;
    if (cfg.output_id != nullptr) dc.outputId = cfg.output_id;
    dc.sampleRate    = cfg.sample_rate > 0.0 ? cfg.sample_rate : 48000.0;
    dc.blockFrames   = cfg.block_frames;
    dc.exclusiveMode = cfg.exclusive != 0;
    dc.ringBlocks    = cfg.ring_blocks == 0.0 ? rt::kDefaultRingBlocks : cfg.ring_blocks;
    if (!rt::validRingBlocks(dc.ringBlocks)) {
        s->lastError = std::format("ring margin must be 1.0 to 2.0 blocks, got {}", cfg.ring_blocks);
        return RT_E_ARG;
    }
    return RT_OK;
}

} // namespace

extern "C" {

rt_session* rt_session_create(void) {
    try { return new rt_session{}; } catch (...) { return nullptr; }
}

void rt_session_destroy(rt_session* s) { delete s; }

int32_t rt_session_open(rt_session* s, const rt_open_config* cfg) {
    if (s == nullptr || cfg == nullptr) return RT_E_ARG;
    try {
        const auto backend = parseBackend(s, cfg->backend);
        if (!backend) return RT_E_ARG;
        rt::DeviceConfig dc;
        if (const int32_t rc = toDeviceConfig(s, *cfg, dc); rc != RT_OK) return rc;
        s->session.open(*backend, dc);
        return RT_OK;
    } catch (const rt::SessionStateError& e) {
        s->lastError = e.what();
        return RT_E_STATE;
    } catch (const std::exception& e) {
        s->lastError = e.what();
        return RT_E_DEVICE;
    } catch (...) {
        s->lastError = "unknown exception";
        return RT_E_DEVICE;
    }
}

int32_t rt_session_stop(rt_session* s) {
    return guarded(s, [](rt::Session& session) { session.stop(); });
}

size_t rt_session_last_error(const rt_session* s, char* buf, size_t cap) {
    if (s == nullptr) return 0;
    if (buf != nullptr && cap > 0) rt::copyUtf8Truncated(buf, cap, s->lastError);
    return s->lastError.size();
}

int32_t rt_session_device(const rt_session* s, rt_device_desc* out) {
    if (out == nullptr) return RT_E_ARG;
    return guarded(s, [&](const rt::Session& session) {
        const rt::DeviceStatus st = session.deviceStatus();
        *out = rt_device_desc{};
        rt::copyUtf8Truncated(out->backend, sizeof out->backend, st.backendName);
        rt::copyUtf8Truncated(out->input,   sizeof out->input,   st.inputName);
        rt::copyUtf8Truncated(out->output,  sizeof out->output,  st.outputName);
        out->sample_rate    = st.sampleRate;
        out->claimed_rtt_ms = st.estimatedRoundTripMs;
        out->block_frames   = st.blockFrames;
        out->channels       = st.numChannels;
    });
}

int32_t rt_session_strip_count(const rt_session* s) {
    if (s == nullptr) return -1;
    return static_cast<int32_t>(s->session.stripCount());
}

int32_t rt_session_strip(const rt_session* s, int32_t strip, rt_strip_desc* out) {
    if (out == nullptr || strip < 0) return RT_E_ARG;
    return guarded(s, [&](const rt::Session& session) {
        const auto i = static_cast<std::size_t>(strip);
        out->name           = session.stripName(i);
        out->param_count    = static_cast<int32_t>(session.stripParams(i).size());
        out->latency_frames = static_cast<int32_t>(session.stripLatency(i));
    });
}

int32_t rt_session_param(const rt_session* s, int32_t strip, int32_t param, rt_param_desc* out) {
    if (out == nullptr || strip < 0 || param < 0) return RT_E_ARG;
    return guarded(s, [&](const rt::Session& session) {
        const auto st = static_cast<std::size_t>(strip);
        const auto p  = static_cast<std::size_t>(param);
        const auto info = session.stripParams(st);
        if (p >= info.size()) throw rt::SessionArgError("parameter index out of range");
        const rt::ParamInfo& pi = info[p];
        out->id    = pi.id;
        out->name  = pi.name;
        out->unit  = pi.unit;
        out->min   = pi.min;
        out->max   = pi.max;
        out->def   = session.paramDefault(st, p);
        out->taper = static_cast<uint8_t>(pi.taper);
        out->flags = pi.flags;
    });
}

int32_t rt_session_set_param(rt_session* s, int32_t strip, int32_t param, double value) {
    if (strip < 0 || param < 0) return RT_E_ARG;
    return guarded(s, [&](rt::Session& session) {
        session.setParam(static_cast<std::size_t>(strip), static_cast<std::size_t>(param), value);
    });
}

int32_t rt_session_snapshot(rt_session* s, rt_snapshot* out) {
    if (out == nullptr) return RT_E_ARG;
    return guarded(s, [&](rt::Session& session) { session.snapshot(*out); });
}

int32_t rt_session_enumerate(rt_session* s, rt_device_info* out, int32_t cap, int32_t* total) {
    if (s == nullptr || total == nullptr || cap < 0 || (out == nullptr && cap > 0)) return RT_E_ARG;
    try {
        const std::vector<rt::DeviceInfo> list = s->session.enumerate();
        int32_t n = 0;
        rt_device_info info{};
        for (const rt::DeviceInfo& d : list) {
            if (!rt::toDeviceInfo(d, info)) continue;
            if (n < cap) out[n] = info;
            ++n;
        }
        *total = n;
        return RT_OK;
    } catch (const rt::SessionStateError& e) {
        s->lastError = e.what();
        return RT_E_STATE;
    } catch (const std::exception& e) {
        s->lastError = e.what();
        return RT_E_DEVICE;
    } catch (...) {
        s->lastError = "unknown exception";
        return RT_E_DEVICE;
    }
}

int32_t rt_session_config(const rt_session* s, rt_config_desc* out) {
    if (out == nullptr) return RT_E_ARG;
    return guarded(s, [&](const rt::Session& session) {
        const rt::DeviceConfig& c = session.config();
        *out = rt_config_desc{};
        rt::copyUtf8Truncated(out->backend, sizeof out->backend, rt::backendKey(session.backend()));
        rt::copyUtf8Truncated(out->input_id, sizeof out->input_id, c.inputId);
        rt::copyUtf8Truncated(out->output_id, sizeof out->output_id, c.outputId);
        out->sample_rate  = c.sampleRate;
        out->block_frames = static_cast<int32_t>(c.blockFrames);
        out->exclusive    = static_cast<uint8_t>(c.exclusiveMode);
        out->ring_blocks  = c.ringBlocks;
    });
}

int32_t rt_session_caps(const rt_session* s, rt_backend_caps* out) {
    if (out == nullptr) return RT_E_ARG;
    return guarded(s, [&](const rt::Session& session) {
        const rt::BackendCaps c = rt::backendCaps(session.backend());
        *out = rt_backend_caps{};
        out->flags = (c.ring ? RT_CAP_RING : 0u) | (c.oneDriver ? RT_CAP_ONE_DRIVER : 0u) |
                     (c.driverPanel ? RT_CAP_DRIVER_PANEL : 0u) |
                     (c.exclusiveMode ? RT_CAP_EXCLUSIVE_MODE : 0u) |
                     (c.rateFromDevice ? RT_CAP_RATE_FROM_DEVICE : 0u) |
                     (c.blockZeroPreferred ? RT_CAP_BLOCK_ZERO_PREFERRED : 0u) |
                     (c.blockRounded ? RT_CAP_BLOCK_ROUNDED : 0u);
        rt::copyUtf8Truncated(out->display_name, sizeof out->display_name, c.displayName);
        rt::copyUtf8Truncated(out->notice, sizeof out->notice, c.notice);
    });
}

int32_t rt_session_reconfigure(rt_session* s, const rt_open_config* cfg, int32_t* outcome) {
    if (s == nullptr || cfg == nullptr || outcome == nullptr) return RT_E_ARG;
    try {
        const auto backend = parseBackend(s, cfg->backend);
        if (!backend) return RT_E_ARG;
        rt::DeviceConfig dc;
        if (const int32_t rc = toDeviceConfig(s, *cfg, dc); rc != RT_OK) return rc;
        if (cfg->backend != nullptr && rt::resolveBackend(*backend) != s->session.backend()) {
            s->lastError = "changing the backend is not supported";
            return RT_E_ARG;
        }
        const rt::ReconfigureResult r = s->session.reconfigure(dc);
        *outcome = static_cast<int32_t>(r);
        if (r == rt::ReconfigureResult::Applied) return RT_OK;
        s->lastError = s->session.reconfigureMessage();
        return RT_E_DEVICE;
    } catch (const rt::SessionStateError& e) {
        s->lastError = e.what();
        return RT_E_STATE;
    } catch (const std::exception& e) {
        s->lastError = e.what();
        return RT_E_INTERNAL;
    } catch (...) {
        s->lastError = "unknown exception";
        return RT_E_INTERNAL;
    }
}

int32_t rt_session_control_panel(rt_session* s, int32_t* result) {
    if (s == nullptr || result == nullptr) return RT_E_ARG;
    try {
        switch (s->session.controlPanel()) {
        case rt::PanelResult::Modal:         *result = RT_PANEL_MODAL; break;
        case rt::PanelResult::AlreadyOpen:   *result = RT_PANEL_ALREADY_OPEN; break;
        case rt::PanelResult::NoDriverPanel: *result = RT_PANEL_NONE; break;
        default:                             *result = RT_PANEL_OPENED; break;
        }
        return RT_OK;
    } catch (const rt::SessionStateError& e) {
        s->lastError = e.what();
        return RT_E_STATE;
    } catch (const std::exception& e) {
        s->lastError = e.what();
        return RT_E_DEVICE;
    } catch (...) {
        s->lastError = "unknown exception";
        return RT_E_DEVICE;
    }
}

int32_t rt_session_latency_enter(rt_session* s) {
    return guarded(s, [](rt::Session& session) { session.latencyEnter(); });
}

int32_t rt_session_latency_leave(rt_session* s) {
    return guarded(s, [](rt::Session& session) { session.latencyLeave(); });
}

int32_t rt_session_latency_start(rt_session* s, int32_t kind, const rt_latency_settings* settings) {
    if (s == nullptr || settings == nullptr) return RT_E_ARG;
    if (kind != RT_LAT_CONTROL && kind != RT_LAT_MEASURE) {
        s->lastError = "unknown latency kind: " + std::to_string(kind);
        return RT_E_ARG;
    }
    return guarded(s, [&](rt::Session& session) {
        rt::LatencySettings ls;
        ls.repeats   = settings->repeats;
        ls.amplitude = settings->amplitude;
        session.latencyStart(static_cast<rt::LatencyKind>(kind), ls);
    });
}

int32_t rt_session_latency_cancel(rt_session* s) {
    return guarded(s, [](rt::Session& session) {
        if (!session.isOpen()) throw rt::SessionStateError("session is not open");
        session.latencyCancel();
    });
}

int32_t rt_session_latency_status(const rt_session* s, rt_latency_status* out) {
    if (out == nullptr) return RT_E_ARG;
    return guarded(s, [&](const rt::Session& session) { session.latencyStatus(*out); });
}

uint64_t rt_hist_percentile_ns(const uint64_t counts[RT_HIST_BUCKETS], double p) {
    if (counts == nullptr) return 0;
    rt::HistogramSnapshot h;
    for (std::size_t b = 0; b < h.counts.size(); ++b) {
        h.counts[b] = counts[b];
        h.total += counts[b];
    }
    return h.nsAtPercentile(p);
}

uint64_t rt_hist_bucket_upper_ns(int32_t bucket) {
    return rt::RtHistogram::bucketUpperNs(bucket);
}

} // extern "C"
