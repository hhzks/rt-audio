#include "ffi/Session.h"
#include "ffi/Utf8.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace rt {

static_assert(RT_HIST_BUCKETS == RtHistogram::kBucketCount);
static_assert(RT_MAX_CHANNELS == kMaxChannels);
static_assert(RT_TAPER_LINEAR == static_cast<int>(Taper::Linear));
static_assert(RT_TAPER_LOG == static_cast<int>(Taper::Log));
static_assert(RT_FLAG_READ_ONLY == kReadOnly && RT_FLAG_TOGGLE == kToggle);

namespace {

constexpr std::array<ParamInfo, 3> kMasterInfo{{
    {"in_gain",  "in",     "dB", -24.0, 24.0, 0.0, Taper::Linear, 0},
    {"out_gain", "out",    "dB", -60.0, 12.0, 0.0, Taper::Linear, 0},
    {"bypass",   "bypass", "",     0.0,  1.0, 0.0, Taper::Linear, kToggle},
}};

} // namespace

Session::Session() : master_(kMasterInfo) {
    rig_ = buildRigChain(engine_.chain());
    applyMaster();
}

Session::~Session() { stop(); }

void Session::open(Backend backend, const DeviceConfig& config) {
    if (device_) throw SessionStateError("session is already open");
    auto device = createAudioDevice(backend);
    device->open(config, &callback_);
    const DeviceStatus st = device->status();
    engine_.prepare(st.sampleRate, st.blockFrames, st.numChannels);
    device->start();
    device_ = std::move(device);
}

void Session::stop() noexcept {
    if (!device_) return;
    try { device_->stop(); } catch (...) {}
}

std::size_t Session::stripCount() const noexcept { return 1 + engine_.chain().size(); }

const IEffect& Session::effect(std::size_t strip) const {
    if (strip == 0 || strip >= stripCount()) throw SessionArgError("strip index out of range");
    return *engine_.chain().at(strip - 1);
}

const char* Session::stripName(std::size_t strip) const {
    return strip == 0 ? "Master" : effect(strip).name();
}

std::span<const ParamInfo> Session::stripParams(std::size_t strip) const {
    return strip == 0 ? master_.info() : effect(strip).params();
}

FrameCount Session::stripLatency(std::size_t strip) const {
    return strip == 0 ? 0 : effect(strip).latencyFrames();
}

void Session::checkParam(std::size_t strip, std::size_t param) const {
    if (param >= stripParams(strip).size()) throw SessionArgError("parameter index out of range");
}

double Session::paramDefault(std::size_t strip, std::size_t param) const {
    checkParam(strip, param);
    return strip == 0 ? master_.defaultValue(param) : effect(strip).paramDefault(param);
}

double Session::getParam(std::size_t strip, std::size_t param) const {
    checkParam(strip, param);
    return strip == 0 ? master_.get(param) : effect(strip).getParam(param);
}

void Session::setParam(std::size_t strip, std::size_t param, double value) {
    checkParam(strip, param);
    if (strip == 0) {
        if (!master_.set(param, value)) throw SessionArgError("value rejected");
        applyMaster();
        return;
    }
    if (!engine_.chain().at(strip - 1)->setParam(param, value))
        throw SessionArgError("value rejected");
}

void Session::applyMaster() noexcept {
    ParameterStore& p = engine_.params();
    p.inputGain.store(static_cast<float>(std::pow(10.0, master_.get(kInGain) / 20.0)),
                      std::memory_order_relaxed);
    p.outputGain.store(static_cast<float>(std::pow(10.0, master_.get(kOutGain) / 20.0)),
                       std::memory_order_relaxed);
    p.bypass.store(master_.get(kBypass) >= 0.5, std::memory_order_relaxed);
}

DeviceStatus Session::deviceStatus() const {
    if (!device_) throw SessionStateError("session is not open");
    return device_->status();
}

void Session::snapshot(rt_snapshot& out) {
    if (!device_) throw SessionStateError("session is not open");
    out = rt_snapshot{};

    RtStats& s = engine_.stats();
    const DeviceStatus ds = device_->status();
    out.callbacks        = s.callbackCount.load(std::memory_order_relaxed);
    out.engine_xruns     = s.xruns.load(std::memory_order_relaxed);
    out.device_xruns     = ds.xruns;
    out.capture_overruns = ds.captureOverruns;
    out.capture_underruns = ds.captureUnderruns;
    out.in_clips         = s.inputClips.load(std::memory_order_relaxed);
    out.out_clips        = s.outputClips.load(std::memory_order_relaxed);
    out.deadline_ns      = s.blockDeadlineNanos.load(std::memory_order_relaxed);

    const HistogramSnapshot h = s.callbackNanos.drain();
    for (std::size_t b = 0; b < h.counts.size(); ++b) out.hist_window[b] = h.counts[b];

    const int channels = std::min(engine_.numChannels(), static_cast<int>(RT_MAX_CHANNELS));
    out.channels = channels;
    for (int c = 0; c < channels; ++c) {
        out.in_peak[c]  = s.inputPeak[idx(c)].exchange(0.0f, std::memory_order_relaxed);
        out.out_peak[c] = s.outputPeak[idx(c)].exchange(0.0f, std::memory_order_relaxed);
    }

    const std::size_t strips = std::min<std::size_t>(stripCount(), RT_MAX_STRIPS);
    for (std::size_t st = 0; st < strips; ++st) {
        const std::size_t n = std::min<std::size_t>(stripParams(st).size(), RT_MAX_PARAMS);
        for (std::size_t p = 0; p < n; ++p) out.params[st][p] = getParam(st, p);
    }

    out.running = static_cast<std::uint8_t>(device_->isRunning() ? 1 : 0);
    copyUtf8Truncated(out.device_error, sizeof out.device_error, ds.lastError);
}

} // namespace rt
