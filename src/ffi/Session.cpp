#include "ffi/Session.h"
#include "ffi/Utf8.h"
#include "dsp/NoiseGate.h"
#include "dsp/Waveshaper.h"
#include "engine/LatencyRun.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <new>
#include <sstream>
#include <string>
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

std::string formatMs(double ms) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << ms;
    return os.str();
}

} // namespace

Session::Session() : Session(DeviceMaker([](Backend b) { return createAudioDevice(b); })) {}

Session::Session(DeviceMaker make) : master_(kMasterInfo), make_(std::move(make)) {
    rig_ = buildRigChain(engine_.chain());
    applyMaster();
}

Session::~Session() {
    latencyCancel();
    if (worker_.joinable()) worker_.join();
    stop();
}

void Session::checkOpened() const {
    if (!opened_) throw SessionStateError("session is not open");
}

std::unique_ptr<IAudioDevice> Session::makeAndStart(const DeviceConfig& config) {
    auto device = make_(backend_);
    if (!device) throw std::runtime_error("device factory returned no device");
    device->open(config, &callback_);
    const DeviceStatus st = device->status();
    engine_.prepare(st.sampleRate, st.blockFrames, st.numChannels);
    device->start();
    return device;
}

std::optional<std::string> Session::tryStart(const DeviceConfig& config) {
    try {
        device_ = makeAndStart(config);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        return std::string(e.what());
    } catch (...) {
        return std::string("unknown error");
    }
    return std::nullopt;
}

void Session::destroyDevice() noexcept {
    if (!device_) return;
    try { device_->stop(); } catch (...) {}
    try { device_->close(); } catch (...) {}
    device_.reset();
}

void Session::open(Backend backend, const DeviceConfig& config) {
    if (opened_) throw SessionStateError("session is already open");
    backend_ = resolveBackend(backend);
    device_  = makeAndStart(config);
    config_  = config;
    opened_  = true;
}

ReconfigureResult Session::reconfigure(const DeviceConfig& next) {
    checkOpened();
    if (running_.load()) throw SessionStateError("a latency measurement is running");
    destroyDevice();

    const auto e1 = tryStart(next);
    if (!e1) {
        config_ = next;
        stopReason_.clear();
        message_.clear();
        return ReconfigureResult::Applied;
    }
    if (next == config_) {
        stopReason_ = message_ = "could not reopen the device: " + *e1;
        return ReconfigureResult::Stopped;
    }
    const std::string first = "could not open the new config: " + *e1;
    const auto e2 = tryStart(config_);
    if (!e2) {
        stopReason_.clear();
        message_ = first + "; restored the previous config";
        return ReconfigureResult::RolledBack;
    }
    stopReason_ = message_ = first + "; could not restore the previous config: " + *e2;
    return ReconfigureResult::Stopped;
}

std::vector<DeviceInfo> Session::enumerate() {
    checkOpened();
    auto probe = make_(backend_);
    if (!probe) throw std::runtime_error("device factory returned no device");
    return probe->enumerate();
}

void Session::stop() noexcept {
    if (!device_) return;
    try { device_->stop(); } catch (...) {}
}

const DeviceConfig& Session::config() const {
    checkOpened();
    return config_;
}

Backend Session::backend() const {
    checkOpened();
    return backend_;
}

void Session::latencyEnter() {
    checkOpened();
    if (running_.load()) throw SessionStateError("a latency measurement is running");
    latencyMode_.store(true, std::memory_order_relaxed);
    callback_.setMode(CallbackMode::Silent);
    std::lock_guard lock(latencyMutex_);
    latency_ = rt_latency_status{};
}

void Session::latencyLeave() {
    checkOpened();
    if (running_.load()) throw SessionStateError("a latency measurement is running");
    latencyMode_.store(false, std::memory_order_relaxed);
    callback_.setMode(CallbackMode::Normal);
    std::lock_guard lock(latencyMutex_);
    latency_ = rt_latency_status{};
}

void Session::latencyStart(LatencyKind kind, const LatencySettings& settings) {
    checkOpened();
    if (!latencyMode_.load(std::memory_order_relaxed))
        throw SessionStateError("not in latency mode");
    if (running_.load()) throw SessionStateError("a latency measurement is running");
    if (!device_ || !device_->isRunning()) throw SessionStateError("the device is stopped");
    if (settings.repeats < 1 || settings.repeats > RT_LAT_MAX_REPEATS)
        throw SessionArgError("repeats must be between 1 and "
                              + std::to_string(RT_LAT_MAX_REPEATS));
    if (!std::isfinite(settings.amplitude) || settings.amplitude <= 0.0f
        || settings.amplitude > 1.0f)
        throw SessionArgError("amplitude must be in (0, 1]");
    {
        std::lock_guard lock(latencyMutex_);
        if (kind == LatencyKind::Measure && !controlPassedLocked(config_))
            throw SessionStateError("run the negative control first");
        latency_ = rt_latency_status{};
        latency_.state   = RT_LAT_RUNNING;
        latency_.kind    = static_cast<std::int32_t>(kind);
        latency_.phase   = RT_LAT_PHASE_DIRECT;
        latency_.repeats = settings.repeats;
    }
    running_.store(true);
    try {
        worker_ = std::jthread([this, kind, settings](std::stop_token stop) {
            runLatency(kind, settings, std::move(stop));
        });
    } catch (...) {
        running_.store(false);
        throw;
    }
}

void Session::latencyCancel() noexcept { worker_.request_stop(); }

void Session::latencyStatus(rt_latency_status& out) const {
    checkOpened();
    std::lock_guard lock(latencyMutex_);
    out = latency_;
    out.latency_mode   = static_cast<std::uint8_t>(latencyMode_.load(std::memory_order_relaxed));
    out.control_passed = static_cast<std::uint8_t>(controlPassedLocked(config_));
}

bool Session::controlPassedLocked(const DeviceConfig& config) const {
    const DevicePair pair{backend_, config.inputId, config.outputId};
    return std::find(controlPassed_.begin(), controlPassed_.end(), pair) != controlPassed_.end();
}

std::uint64_t Session::dropoutCount() {
    const DeviceStatus d = device_->status();
    return d.captureOverruns + d.captureUnderruns + d.xruns
         + engine_.stats().xruns.load(std::memory_order_relaxed);
}

void Session::drainCallbacks() noexcept {
    const auto& count = engine_.stats().callbackCount;
    const std::uint64_t start = count.load(std::memory_order_relaxed);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (count.load(std::memory_order_relaxed) < start + 2
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void Session::runLatency(LatencyKind kind, LatencySettings settings, std::stop_token stop) {
    const DeviceStatus st = device_->status();
    const DevicePair pair{backend_, config_.inputId, config_.outputId};
    const double bypass = master_.get(kBypass);
    const double gateOn = rig_.gate->getParam(NoiseGate::kOn);
    const double mix    = rig_.shaper->getParam(Waveshaper::kMix);
    const double drive  = rig_.shaper->getParam(Waveshaper::kDrive);
    bool neutral = false;
    std::int32_t finalState = RT_LAT_FAILED;
    std::string message;

    try {
        SweepConfig sweep;
        sweep.amplitude = settings.amplitude;
        probe_.prepare(st.sampleRate, st.blockFrames, st.numChannels, sweep);

        PhaseConfig cfg;
        cfg.repeats      = settings.repeats;
        cfg.maxLagFrames = static_cast<int>(sweep.maxLatencySeconds * st.sampleRate);
        cfg.sampleRate   = st.sampleRate;
        cfg.maxWarmup    = kind == LatencyKind::Control ? 0 : kMeasurementWarmup;

        std::int32_t phase = RT_LAT_PHASE_DIRECT;
        PhaseHooks hooks;
        hooks.dropouts  = [this] { return dropoutCount(); };
        hooks.cancelled = [&stop] { return stop.stop_requested(); };
        hooks.alive     = [this] { return device_->isRunning(); };
        hooks.progress  = [this, &phase](PhaseEvent e, int repeat, int) {
            if (e != PhaseEvent::Attempt) return;
            std::lock_guard lock(latencyMutex_);
            latency_.phase  = phase;
            latency_.repeat = repeat;
        };

        callback_.setMode(CallbackMode::ProbeDirect);
        const PhaseResult a = runPhase(probe_, cfg, hooks);
        {
            std::lock_guard lock(latencyMutex_);
            latency_.kept        = static_cast<std::int32_t>(a.kept.size());
            latency_.discarded   = a.discarded;
            latency_.measured_ms = a.medianMs();
            latency_.spread_ms   = a.spreadMs();
            latency_.computed_ms = st.estimatedRoundTripMs;
            latency_.clipped     = static_cast<std::uint8_t>(a.anyClipped);
            const std::size_t n = std::min<std::size_t>(a.kept.size(), RT_LAT_MAX_REPEATS);
            for (std::size_t i = 0; i < n; ++i) {
                const LatencyResult& r = a.kept[i];
                latency_.direct[i] = rt_latency_repeat{
                    r.lagMs, r.peakCorrelation, r.peakToSidelobe,
                    static_cast<std::uint8_t>(r.valid),
                    static_cast<std::uint8_t>(r.polarityInverted)};
            }
        }

        if (kind == LatencyKind::Control) {
            std::lock_guard lock(latencyMutex_);
            if (!a.enoughKept()) {
                message = "too many xruns to trust the control";
            } else if (a.anyValid()) {
                double worst = 0.0;
                for (const LatencyResult& r : a.kept)
                    if (r.valid) worst = std::max(worst, r.lagMs);
                message = "detected a peak at " + formatMs(worst)
                        + " ms with no physical path; a software route (VoiceMeeter, Sonar, "
                          "VB-Audio, a virtual cable or Stereo Mix) connects the devices";
                std::erase(controlPassed_, pair);
            } else {
                if (std::find(controlPassed_.begin(), controlPassed_.end(), pair)
                    == controlPassed_.end())
                    controlPassed_.push_back(pair);
                message = "control passed: no peak without a physical path";
                finalState = RT_LAT_DONE;
            }
        } else if (!a.passedMajority()) {
            message = std::to_string(a.kept.size()) + " of " + std::to_string(settings.repeats)
                    + " repeats kept (" + std::to_string(a.discarded)
                    + " discarded to xruns), " + std::to_string(a.validCount())
                    + " found a peak. Raise the level, turn off direct monitoring, make sure "
                      "the earcup touches the capsule.";
        } else {
            neutral = true;
            master_.set(kBypass, 0.0);
            applyMaster();
            rig_.gate->setParam(NoiseGate::kOn, 0.0);
            rig_.shaper->setParam(Waveshaper::kMix, 0.0);
            rig_.shaper->setParam(Waveshaper::kDrive, 1.0);
            phase = RT_LAT_PHASE_CHAIN;
            callback_.setMode(CallbackMode::ProbeChain);
            const PhaseResult b = runPhase(probe_, cfg, hooks);

            std::lock_guard lock(latencyMutex_);
            latency_.chain_valid = static_cast<std::uint8_t>(b.passedMajority());
            if (b.passedMajority()) latency_.chain_measured_ms = b.medianMs() - a.medianMs();
            latency_.chain_reported_frames =
                static_cast<std::int32_t>(engine_.chain().totalLatencyFrames());
            if (b.anyClipped) latency_.clipped = 1;
            if (latency_.clipped != 0) message = "the capture clipped; lower the level";
            finalState = RT_LAT_DONE;
        }
    } catch (const PhaseCancelled&) {
        finalState = RT_LAT_CANCELLED;
    } catch (const std::bad_alloc&) {
        message = "out of memory";
    } catch (const std::exception& e) {
        message = e.what();
    } catch (...) {
        message = "unknown error";
    }

    // Run-end rule: after this, no callback is inside probe_.process(), so the next
    // prepare() and arm() cannot race the audio thread.
    callback_.setMode(CallbackMode::Silent);
    drainCallbacks();
    if (neutral) {
        master_.set(kBypass, bypass);
        applyMaster();
        rig_.gate->setParam(NoiseGate::kOn, gateOn);
        rig_.shaper->setParam(Waveshaper::kMix, mix);
        rig_.shaper->setParam(Waveshaper::kDrive, drive);
    }
    {
        std::lock_guard lock(latencyMutex_);
        latency_.state = finalState;
        copyUtf8Truncated(latency_.message, sizeof latency_.message, message);
    }
    running_.store(false);
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
    checkOpened();
    if (!device_) throw SessionStateError("no device is open: " + stopReason_);
    return device_->status();
}

void Session::snapshot(rt_snapshot& out) {
    checkOpened();
    out = rt_snapshot{};

    RtStats& s = engine_.stats();
    const DeviceStatus ds = device_ ? device_->status() : DeviceStatus{};
    out.callbacks         = s.callbackCount.load(std::memory_order_relaxed);
    out.engine_xruns      = s.xruns.load(std::memory_order_relaxed);
    out.device_xruns      = ds.xruns;
    out.capture_overruns  = ds.captureOverruns;
    out.capture_underruns = ds.captureUnderruns;
    out.in_clips          = s.inputClips.load(std::memory_order_relaxed);
    out.out_clips         = s.outputClips.load(std::memory_order_relaxed);
    out.deadline_ns       = s.blockDeadlineNanos.load(std::memory_order_relaxed);

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

    out.running = static_cast<std::uint8_t>(device_ && device_->isRunning() ? 1 : 0);
    copyUtf8Truncated(out.device_error, sizeof out.device_error,
                      device_ ? ds.lastError : stopReason_);
}

} // namespace rt
