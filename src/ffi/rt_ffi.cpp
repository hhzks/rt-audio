#include "ffi/rt_ffi.h"
#include "ffi/Session.h"
#include "ffi/Utf8.h"
#include "engine/RtHistogram.h"

#include <exception>
#include <string>

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

} // namespace

extern "C" {

rt_session* rt_session_create(void) {
    try { return new rt_session{}; } catch (...) { return nullptr; }
}

void rt_session_destroy(rt_session* s) { delete s; }

int32_t rt_session_open(rt_session* s, const rt_open_config* cfg) {
    if (s == nullptr || cfg == nullptr) return RT_E_ARG;
    try {
        rt::Backend backend = rt::Backend::Default;
        if (cfg->backend != nullptr) {
            const auto b = rt::backendFromName(cfg->backend);
            if (!b) {
                s->lastError = std::string("unknown backend: ") + cfg->backend;
                return RT_E_ARG;
            }
            backend = *b;
        }
        rt::DeviceConfig dc;
        if (cfg->input_id != nullptr)  dc.inputId  = cfg->input_id;
        if (cfg->output_id != nullptr) dc.outputId = cfg->output_id;
        dc.sampleRate    = cfg->sample_rate > 0.0 ? cfg->sample_rate : 48000.0;
        dc.blockFrames   = cfg->block_frames;
        dc.exclusiveMode = cfg->exclusive != 0;
        s->session.open(backend, dc);
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
