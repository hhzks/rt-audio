#include "ffi/Session.h"
#include "ffi/Utf8.h"
#include "dsp/Biquad.h"
#include "dsp/NoiseGate.h"
#include "dsp/Waveshaper.h"
#include "DelayLine.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace rt;
using Catch::Matchers::WithinAbs;
using rt::testing::DelayLine;

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

using Log = std::vector<std::string>;

struct Script {
    Log                   log;
    std::set<std::string> failing;   // input ids whose open() throws
    char                  next = 'A';
};

class ScriptedDevice : public IAudioDevice {
public:
    ScriptedDevice(Script& script, std::string name) : script_(script), name_(std::move(name)) {
        script_.log.push_back(name_ + ".make");
    }
    ~ScriptedDevice() override {
        halt();
        script_.log.push_back(name_ + ".~");
    }
    ScriptedDevice(const ScriptedDevice&) = delete;
    ScriptedDevice& operator=(const ScriptedDevice&) = delete;

    std::vector<DeviceInfo> enumerate() override {
        DeviceInfo mic;
        mic.id = "mic";
        mic.name = "Mic";
        mic.maxInputChannels = 2;
        DeviceInfo phones;
        phones.id = "phones";
        phones.name = "Phones";
        phones.maxOutputChannels = 2;
        return {mic, phones};
    }

    void open(const DeviceConfig& config, IAudioCallback* callback) override {
        script_.log.push_back(name_ + ".open");
        if (script_.failing.contains(config.inputId))
            throw std::runtime_error("cannot open " + config.inputId);
        config_   = config;
        callback_ = callback;
        if (config_.blockFrames <= 0) config_.blockFrames = 256;
        in_.assign(idx(config_.blockFrames) * idx(config_.numChannels), 0.1f);
        out_.assign(in_.size(), 0.0f);
        status_.sampleRate  = config_.sampleRate;
        status_.blockFrames = config_.blockFrames;
        status_.numChannels = config_.numChannels;
        status_.backendName = "Scripted " + name_;
    }

    void start() override {
        script_.log.push_back(name_ + ".start");
        running_ = true;
        thread_ = std::thread([this] {
            while (running_.load()) {
                callback_->audioDeviceProcess(in_.data(), out_.data(), config_.blockFrames);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    void stop() override  { script_.log.push_back(name_ + ".stop");  halt(); }
    void close() override { script_.log.push_back(name_ + ".close"); halt(); }
    DeviceStatus status() const override { return status_; }
    bool isRunning() const override { return running_.load(); }

private:
    void halt() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    Script&            script_;
    std::string        name_;
    DeviceConfig       config_{};
    DeviceStatus       status_{};
    IAudioCallback*    callback_ = nullptr;
    std::vector<float> in_, out_;
    std::atomic<bool>  running_{false};
    std::thread        thread_;
};

Session::DeviceMaker scripted(Script& script) {
    return [&script](Backend) -> std::unique_ptr<IAudioDevice> {
        const std::string name(1, script.next);
        ++script.next;
        return std::make_unique<ScriptedDevice>(script, name);
    };
}

DeviceConfig scriptedConfig(const char* input) {
    DeviceConfig c = nullConfig();
    c.inputId = input;
    return c;
}

Log since(const Script& script, std::size_t mark) {
    return Log(script.log.begin() + static_cast<std::ptrdiff_t>(mark), script.log.end());
}

struct LoopbackRig {
    int                delayFrames = 480;
    std::atomic<bool>  connected{true};   // false: the input hears nothing of the output
    std::atomic<float> tone{0.0f};        // amplitude of a 1 kHz sine added to the input
    std::atomic<float> outPeak{0.0f};     // max |out| of the last block
    bool               hasPanel = false;
    int                panelOpens = 0;
    PanelResult        panelResult = PanelResult::Opened;
    std::atomic<bool>  panelOpen{false};
    bool               failOpen = false;
    int                makes = 0;
};

// Feeds its output back to its input through a delay line, faster than real time.
class LoopbackDevice : public IAudioDevice {
public:
    explicit LoopbackDevice(LoopbackRig& rig) : rig_(rig) {}
    ~LoopbackDevice() override { halt(); }
    LoopbackDevice(const LoopbackDevice&) = delete;
    LoopbackDevice& operator=(const LoopbackDevice&) = delete;

    std::vector<DeviceInfo> enumerate() override { return {}; }

    void open(const DeviceConfig& config, IAudioCallback* callback) override {
        if (rig_.failOpen) throw std::runtime_error("cannot open");
        config_   = config;
        callback_ = callback;
        if (config_.blockFrames <= 0) config_.blockFrames = 128;
        status_.sampleRate           = config_.sampleRate;
        status_.blockFrames          = config_.blockFrames;
        status_.numChannels          = config_.numChannels;
        status_.backendName          = "Loopback";
        status_.estimatedRoundTripMs = 1.0;
    }
    void start() override {
        running_ = true;
        thread_ = std::thread([this] { run(); });
    }
    void stop() override  { halt(); }
    void close() override { halt(); }
    DeviceStatus status() const override { return status_; }
    bool isRunning() const override { return running_.load(); }

    PanelResult openControlPanel(const DeviceConfig&) override {
        if (!rig_.hasPanel) return PanelResult::Unsupported;
        ++rig_.panelOpens;
        return rig_.panelResult;
    }
    bool panelOpen() const override { return rig_.panelOpen.load(); }

private:
    void run() {
        const int        ch = config_.numChannels;
        const FrameCount n  = config_.blockFrames;
        DelayLine loop(rig_.delayFrames, ch);
        std::vector<float> in(idx(n) * idx(ch)), out(in.size());
        double phase = 0.0;
        const double inc = 2.0 * 3.141592653589793 * 1000.0 / config_.sampleRate;
        while (running_.load()) {
            loop.read(in.data(), n);
            if (!rig_.connected.load()) std::fill(in.begin(), in.end(), 0.0f);
            const float amp = rig_.tone.load();
            for (FrameCount f = 0; f < n; ++f) {
                const float s = static_cast<float>(std::sin(phase)) * amp;
                phase += inc;
                for (int c = 0; c < ch; ++c) in[idx(f * ch + c)] += s;
            }
            callback_->audioDeviceProcess(in.data(), out.data(), n);
            float peak = 0.0f;
            for (float v : out) peak = std::max(peak, std::fabs(v));
            rig_.outPeak.store(peak);
            loop.write(out.data(), n);
            std::this_thread::yield();
        }
    }
    void halt() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    LoopbackRig&      rig_;
    DeviceConfig      config_{};
    DeviceStatus      status_{};
    IAudioCallback*   callback_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread       thread_;
};

Session::DeviceMaker loopback(LoopbackRig& rig) {
    return [&rig](Backend) -> std::unique_ptr<IAudioDevice> {
        ++rig.makes;
        return std::make_unique<LoopbackDevice>(rig);
    };
}

struct TapScript {
    Log                log;
    bool               throwOnStart = false;
    float              add = 0.0f;
    std::atomic<int>   pulls{0};
    std::atomic<float> gain{-1.0f};
    SystemAudioStatus  status;
    float              peak = 0.0f;
};

class FakeTap : public ISystemAudioTap {
public:
    explicit FakeTap(TapScript& s) : s_(s) {}
    std::vector<SystemSource> sources() override {
        return {{"spk", "Speakers"}, {"cable", "CABLE Input"}};
    }
    void start(const TapInputs& in, double, int channels, FrameCount) override {
        if (s_.throwOnStart) throw std::runtime_error("tap failed");
        channels_ = channels;
        s_.log.push_back("start:" + in.sourceId);
    }
    void stop() noexcept override { s_.log.push_back("stop"); }
    void pull(float* out, FrameCount n, float gain) noexcept override {
        s_.pulls.fetch_add(1);
        s_.gain.store(gain);
        for (std::size_t k = 0; k < idx(n) * idx(channels_); ++k) out[k] += s_.add * gain;
    }
    float takePeak() noexcept override { return s_.peak; }
    SystemAudioStatus status() const override { return s_.status; }

private:
    TapScript& s_;
    int        channels_ = 0;
};

Session::TapMaker fakeTap(TapScript& script) {
    return [&script]() -> std::unique_ptr<ISystemAudioTap> {
        return std::make_unique<FakeTap>(script);
    };
}

LatencySettings threeRepeats() {
    LatencySettings s;
    s.repeats = 3;
    return s;
}

rt_latency_status waitLatency(Session& s) {
    rt_latency_status st{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;) {
        s.latencyStatus(st);
        if (st.state != RT_LAT_RUNNING || std::chrono::steady_clock::now() > deadline) return st;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
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
    CHECK(s.stripParams(0).size() == 4);
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

TEST_CASE("reconfigure applies a new config and keeps params", "[session]") {
    Script script;
    Session s(scripted(script));
    s.setParam(3, Waveshaper::kDrive, 6.0);
    s.open(Backend::Null, scriptedConfig("a"));
    rt_snapshot snap{};
    REQUIRE(waitForCallbacks(s, snap, 3));

    DeviceConfig next = scriptedConfig("b");
    next.blockFrames = 512;
    const std::size_t mark = script.log.size();
    CHECK(s.reconfigure(next) == ReconfigureResult::Applied);
    CHECK(since(script, mark) == Log{"A.stop", "A.close", "A.~", "B.make", "B.open", "B.start"});
    CHECK(s.config() == next);
    CHECK(s.reconfigureMessage().empty());

    REQUIRE(waitForCallbacks(s, snap, 3));
    CHECK(snap.running == 1);
    CHECK(snap.device_error[0] == '\0');
    CHECK_THAT(static_cast<double>(snap.deadline_ns), WithinAbs(512.0 / 48000.0 * 1e9, 2.0));
    CHECK_THAT(snap.params[3][Waveshaper::kDrive], WithinAbs(6.0, 1e-12));
}

TEST_CASE("a failed open rolls back to the previous config", "[session]") {
    Script script;
    script.failing.insert("b");
    Session s(scripted(script));
    const DeviceConfig a = scriptedConfig("a");
    s.open(Backend::Null, a);

    CHECK(s.reconfigure(scriptedConfig("b")) == ReconfigureResult::RolledBack);
    CHECK(s.config() == a);
    CHECK(s.reconfigureMessage().find("cannot open b") != std::string::npos);
    CHECK(s.reconfigureMessage().find("restored the previous config") != std::string::npos);

    rt_snapshot snap{};
    REQUIRE(waitForCallbacks(s, snap, 3));
    CHECK(snap.running == 1);
    CHECK(snap.device_error[0] == '\0');
}

TEST_CASE("when the rollback also fails the session is stopped", "[session]") {
    Script script;
    Session s(scripted(script));
    const DeviceConfig a = scriptedConfig("a");
    s.open(Backend::Null, a);
    script.failing = {"a", "b"};

    CHECK(s.reconfigure(scriptedConfig("b")) == ReconfigureResult::Stopped);
    CHECK(s.config() == a);
    CHECK(s.isOpen());

    rt_snapshot snap{};
    s.snapshot(snap);
    CHECK(snap.running == 0);
    const std::string err = snap.device_error;
    CHECK(err.find("cannot open b") != std::string::npos);
    CHECK(err.find("cannot open a") != std::string::npos);
    CHECK_THROWS_AS(s.deviceStatus(), SessionStateError);
}

TEST_CASE("a retry of the same config makes one attempt, then recovers", "[session]") {
    Script script;
    Session s(scripted(script));
    const DeviceConfig a = scriptedConfig("a");
    s.open(Backend::Null, a);
    script.failing = {"a"};

    const std::size_t mark = script.log.size();
    CHECK(s.reconfigure(a) == ReconfigureResult::Stopped);
    const Log tail = since(script, mark);
    CHECK(std::count_if(tail.begin(), tail.end(),
                        [](const std::string& entry) { return entry.ends_with(".open"); }) == 1);
    CHECK(s.reconfigureMessage().starts_with("could not reopen the device"));

    script.failing.clear();
    CHECK(s.reconfigure(a) == ReconfigureResult::Applied);
    rt_snapshot snap{};
    REQUIRE(waitForCallbacks(s, snap, 3));
    CHECK(snap.running == 1);
    CHECK(snap.device_error[0] == '\0');
}

TEST_CASE("reconfigure, enumerate and config need an open session", "[session]") {
    Session s;
    CHECK(!s.isOpen());
    CHECK_THROWS_AS(s.reconfigure(nullConfig()), SessionStateError);
    CHECK_THROWS_AS(s.enumerate(), SessionStateError);
    CHECK_THROWS_AS(s.config(), SessionStateError);
    CHECK_THROWS_AS(s.backend(), SessionStateError);

    Script script;
    script.failing.insert("a");
    Session t(scripted(script));
    CHECK_THROWS(t.open(Backend::Null, scriptedConfig("a")));
    CHECK(!t.isOpen());
    CHECK_THROWS_AS(t.reconfigure(scriptedConfig("b")), SessionStateError);
}

TEST_CASE("enumerate uses a temporary device", "[session]") {
    Script script;
    Session s(scripted(script));
    s.open(Backend::Null, scriptedConfig("a"));

    const std::size_t mark = script.log.size();
    const std::vector<DeviceInfo> list = s.enumerate();
    CHECK(list.size() == 2);
    CHECK(since(script, mark) == Log{"B.make", "B.~"});
    CHECK(s.backend() == Backend::Null);

    rt_snapshot snap{};
    REQUIRE(waitForCallbacks(s, snap, 3));
    CHECK(snap.running == 1);
}

TEST_CASE("latency mode silences the output and keeps callbacks moving", "[session]") {
    LoopbackRig rig;
    rig.connected = false;
    rig.tone = 0.25f;
    Session s(loopback(rig));
    s.open(Backend::Null, nullConfig());
    rt_snapshot snap{};
    REQUIRE(waitForCallbacks(s, snap, 20));
    CHECK(rig.outPeak.load() > 0.05f);

    s.latencyEnter();
    CHECK(s.latencyMode());
    s.snapshot(snap);
    REQUIRE(waitForCallbacks(s, snap, snap.callbacks + 20));
    CHECK(rig.outPeak.load() == 0.0f);
    CHECK(snap.in_peak[0] > 0.1f);
    CHECK(snap.running == 1);

    s.latencyLeave();
    CHECK(!s.latencyMode());
    s.snapshot(snap);
    REQUIRE(waitForCallbacks(s, snap, snap.callbacks + 20));
    CHECK(rig.outPeak.load() > 0.05f);
}

TEST_CASE("the silent mode survives a reconfigure", "[session]") {
    LoopbackRig rig;
    rig.connected = false;
    rig.tone = 0.25f;
    Session s(loopback(rig));
    s.open(Backend::Null, nullConfig());
    s.latencyEnter();

    DeviceConfig next = nullConfig();
    next.blockFrames = 512;
    CHECK(s.reconfigure(next) == ReconfigureResult::Applied);
    rt_snapshot snap{};
    REQUIRE(waitForCallbacks(s, snap, 20));
    CHECK(rig.outPeak.load() == 0.0f);
    CHECK(s.latencyMode());
}

TEST_CASE("latency mode needs an open session", "[session]") {
    Session s;
    CHECK_THROWS_AS(s.latencyEnter(), SessionStateError);
    CHECK_THROWS_AS(s.latencyLeave(), SessionStateError);
}

TEST_CASE("the control passes only without a physical path", "[session]") {
    LoopbackRig rig;
    rig.connected = false;
    Session s(loopback(rig));
    s.open(Backend::Null, nullConfig());
    s.latencyEnter();

    s.latencyStart(LatencyKind::Control, threeRepeats());
    rt_latency_status st = waitLatency(s);
    CHECK(st.state == RT_LAT_DONE);
    CHECK(st.control_passed == 1);
    CHECK(st.kept == 3);

    rig.connected = true;
    s.latencyStart(LatencyKind::Control, threeRepeats());
    st = waitLatency(s);
    CHECK(st.state == RT_LAT_FAILED);
    CHECK(std::string(st.message).find("detected a peak") != std::string::npos);
    CHECK(st.control_passed == 0);
}

TEST_CASE("a measurement recovers the loop delay and restores the chain", "[session]") {
    LoopbackRig rig;
    rig.connected = false;
    Session s(loopback(rig));
    s.setParam(3, Waveshaper::kDrive, 5.0);
    s.setParam(3, Waveshaper::kMix, 0.7);
    s.open(Backend::Null, nullConfig());
    s.latencyEnter();

    CHECK_THROWS_AS(s.latencyStart(LatencyKind::Measure, threeRepeats()), SessionStateError);
    s.latencyStart(LatencyKind::Control, threeRepeats());
    REQUIRE(waitLatency(s).control_passed == 1);

    rig.connected = true;
    s.latencyStart(LatencyKind::Measure, threeRepeats());
    const rt_latency_status st = waitLatency(s);
    CHECK(st.state == RT_LAT_DONE);
    CHECK_THAT(st.measured_ms, WithinAbs(480.0 / 48.0, 0.05));
    CHECK_THAT(st.computed_ms, WithinAbs(1.0, 1e-9));
    CHECK(st.chain_valid == 1);
    CHECK(st.chain_measured_ms > 0.2);
    CHECK(st.chain_measured_ms < 1.0);
    CHECK(st.chain_reported_frames == 16);

    CHECK_THAT(s.getParam(3, Waveshaper::kDrive), WithinAbs(5.0, 1e-12));
    CHECK_THAT(s.getParam(3, Waveshaper::kMix), WithinAbs(0.7, 1e-12));
    CHECK_THAT(s.getParam(2, NoiseGate::kOn), WithinAbs(1.0, 1e-12));
    CHECK_THAT(s.getParam(0, 2), WithinAbs(0.0, 1e-12));   // Master bypass
}

TEST_CASE("calls that would disturb a run are refused; cancel ends it", "[session]") {
    LoopbackRig rig;
    rig.connected = false;
    Session s(loopback(rig));
    s.open(Backend::Null, nullConfig());
    s.latencyEnter();

    s.latencyStart(LatencyKind::Control, threeRepeats());
    CHECK_THROWS_AS(s.reconfigure(nullConfig()), SessionStateError);
    CHECK_THROWS_AS(s.latencyLeave(), SessionStateError);
    CHECK_THROWS_AS(s.latencyEnter(), SessionStateError);
    CHECK_THROWS_AS(s.latencyStart(LatencyKind::Control, threeRepeats()), SessionStateError);
    s.latencyCancel();
    const rt_latency_status st = waitLatency(s);
    CHECK(st.state == RT_LAT_CANCELLED);

    rt_snapshot snap{};
    s.snapshot(snap);
    REQUIRE(waitForCallbacks(s, snap, snap.callbacks + 20));
    CHECK(rig.outPeak.load() == 0.0f);                     // back to Silent
    s.latencyLeave();
}

TEST_CASE("the control pass follows the device pair", "[session]") {
    LoopbackRig rig;
    rig.connected = false;
    Session s(loopback(rig));
    s.open(Backend::Null, nullConfig());
    s.latencyEnter();
    s.latencyStart(LatencyKind::Control, threeRepeats());
    REQUIRE(waitLatency(s).control_passed == 1);

    DeviceConfig bigger = nullConfig();
    bigger.blockFrames = 512;
    CHECK(s.reconfigure(bigger) == ReconfigureResult::Applied);
    rt_latency_status st{};
    s.latencyStatus(st);
    CHECK(st.control_passed == 1);

    DeviceConfig tighter = bigger;
    tighter.ringBlocks = 1.0;
    CHECK(s.reconfigure(tighter) == ReconfigureResult::Applied);
    s.latencyStatus(st);
    CHECK(st.control_passed == 1);

    DeviceConfig other = bigger;
    other.inputId = "other";
    CHECK(s.reconfigure(other) == ReconfigureResult::Applied);
    s.latencyStatus(st);
    CHECK(st.control_passed == 0);
}

TEST_CASE("a latency start checks the mode, the device and the settings", "[session]") {
    LoopbackRig rig;
    Session s(loopback(rig));
    CHECK_THROWS_AS(s.latencyStart(LatencyKind::Control, LatencySettings{}), SessionStateError);
    s.open(Backend::Null, nullConfig());
    CHECK_THROWS_AS(s.latencyStart(LatencyKind::Control, LatencySettings{}), SessionStateError);
    s.latencyEnter();

    LatencySettings bad;
    bad.repeats = 0;
    CHECK_THROWS_AS(s.latencyStart(LatencyKind::Control, bad), SessionArgError);
    bad.repeats = RT_LAT_MAX_REPEATS + 1;
    CHECK_THROWS_AS(s.latencyStart(LatencyKind::Control, bad), SessionArgError);
    bad.repeats = 5;
    for (float a : {0.0f, 1.5f, std::numeric_limits<float>::quiet_NaN()}) {
        bad.amplitude = a;
        CHECK_THROWS_AS(s.latencyStart(LatencyKind::Control, bad), SessionArgError);
    }

    s.stop();
    CHECK_THROWS_AS(s.latencyStart(LatencyKind::Control, LatencySettings{}), SessionStateError);
}

TEST_CASE("destroying the Session during a run is clean", "[session]") {
    LoopbackRig rig;
    rig.connected = false;
    {
        Session s(loopback(rig));
        s.open(Backend::Null, nullConfig());
        s.latencyEnter();
        s.latencyStart(LatencyKind::Control, threeRepeats());
    }
    SUCCEED();
}

TEST_CASE("the driver panel needs a backend that has one", "[session]") {
    LoopbackRig rig;
    rig.hasPanel = true;
    Session s(loopback(rig));
    CHECK_THROWS_AS(s.controlPanel(), SessionStateError);
    s.open(Backend::Null, nullConfig());
    CHECK_THROWS_WITH(s.controlPanel(), "this backend has no driver panel");
    CHECK(rig.panelOpens == 0);
}

TEST_CASE("the driver panel result comes from the device", "[session]") {
    LoopbackRig rig;
    rig.hasPanel = true;
    rig.panelResult = PanelResult::Modal;
    Session s(loopback(rig));
    s.open(Backend::Asio, nullConfig());
    CHECK(s.controlPanel() == PanelResult::Modal);
    CHECK(rig.panelOpens == 1);
}

TEST_CASE("the driver panel of a stopped device uses that device", "[session]") {
    LoopbackRig rig;
    rig.hasPanel = true;
    Session s(loopback(rig));
    s.open(Backend::Asio, nullConfig());
    s.stop();
    const int makes = rig.makes;
    CHECK(s.controlPanel() == PanelResult::Opened);
    CHECK(rig.makes == makes);
    CHECK(rig.panelOpens == 1);
}

TEST_CASE("the driver panel with no device makes one and keeps the stop reason", "[session]") {
    LoopbackRig rig;
    rig.hasPanel = true;
    Session s(loopback(rig));
    s.open(Backend::Asio, nullConfig());
    rig.failOpen = true;
    DeviceConfig next = nullConfig();
    next.blockFrames = 512;
    REQUIRE(s.reconfigure(next) == ReconfigureResult::Stopped);
    const int makes = rig.makes;
    CHECK(s.controlPanel() == PanelResult::Opened);
    CHECK(rig.makes == makes + 1);
    rt_snapshot snap{};
    s.snapshot(snap);
    CHECK(snap.running == 0);
    CHECK(std::string(snap.device_error).find("could not") != std::string::npos);
}

TEST_CASE("reconfigure refuses while a modal driver panel is open", "[session]") {
    LoopbackRig rig;
    rig.hasPanel = true;
    Session s(loopback(rig));
    s.open(Backend::Asio, nullConfig());
    rig.panelOpen = true;
    rt_snapshot snap{};
    s.snapshot(snap);
    CHECK(snap.panel_open == 1);
    CHECK_THROWS_WITH(s.reconfigure(nullConfig()), "close the driver panel first");
    rig.panelOpen = false;
    CHECK(s.reconfigure(nullConfig()) == ReconfigureResult::Applied);
}

TEST_CASE("a one-driver backend uses the id that is set for input and output", "[session]") {
    LoopbackRig rig;
    Session s(loopback(rig));
    DeviceConfig c = nullConfig();
    c.outputId = "drv";
    s.open(Backend::Asio, c);
    CHECK(s.config().inputId == "drv");
    CHECK(s.config().outputId == "drv");
    DeviceConfig next = nullConfig();
    next.inputId = "other";
    REQUIRE(s.reconfigure(next) == ReconfigureResult::Applied);
    CHECK(s.config().inputId == "other");
    CHECK(s.config().outputId == "other");
}

TEST_CASE("other backends keep an empty id empty", "[session]") {
    LoopbackRig rig;
    Session s(loopback(rig));
    DeviceConfig c = nullConfig();
    c.outputId = "phones";
    s.open(Backend::Null, c);
    CHECK(s.config().inputId.empty());
    CHECK(s.config().outputId == "phones");
}

TEST_CASE("master has a system level after bypass", "[session]") {
    Session s;
    REQUIRE(s.stripParams(0).size() == 4);
    const ParamInfo& p = s.stripParams(0)[3];
    CHECK(std::string(p.id) == "sys_level");
    CHECK(p.min == -60.0);
    CHECK(p.max == 6.0);
    CHECK(p.def == 0.0);
    CHECK((s.stripParams(0)[2].flags & kToggle) != 0);
}

TEST_CASE("the system tap mixes only in the normal mode", "[session]") {
    Script script;
    TapScript tap;
    Session s(scripted(script), fakeTap(tap));
    s.open(Backend::Null, scriptedConfig("a"));
    rt_snapshot snap{};
    REQUIRE(waitForCallbacks(s, snap, 5));
    CHECK(tap.pulls.load() > 0);
    CHECK(tap.log == Log{"start:"});

    s.latencyEnter();
    REQUIRE(waitForCallbacks(s, snap, snap.callbacks + 3));
    const int held = tap.pulls.load();
    REQUIRE(waitForCallbacks(s, snap, snap.callbacks + 5));
    CHECK(tap.pulls.load() == held);

    s.latencyLeave();
    REQUIRE(waitForCallbacks(s, snap, snap.callbacks + 5));
    CHECK(tap.pulls.load() > held);
}

TEST_CASE("the system level sets the tap gain; the bottom is off", "[session]") {
    Script script;
    TapScript tap;
    Session s(scripted(script), fakeTap(tap));
    s.open(Backend::Null, scriptedConfig("a"));
    rt_snapshot snap{};
    REQUIRE(waitForCallbacks(s, snap, 3));
    CHECK_THAT(tap.gain.load(), WithinAbs(1.0, 1e-6));

    s.setParam(0, 3, -6.0);
    REQUIRE(waitForCallbacks(s, snap, snap.callbacks + 3));
    CHECK_THAT(tap.gain.load(), WithinAbs(0.501, 0.001));

    s.setParam(0, 3, -60.0);
    REQUIRE(waitForCallbacks(s, snap, snap.callbacks + 3));
    CHECK(tap.gain.load() == 0.0f);
}

TEST_CASE("a source change restarts the tap and not the device", "[session]") {
    Script script;
    TapScript tap;
    Session s(scripted(script), fakeTap(tap));
    s.open(Backend::Null, scriptedConfig("a"));
    rt_snapshot snap{};
    REQUIRE(waitForCallbacks(s, snap, 3));
    const std::size_t mark = script.log.size();
    tap.log.clear();

    s.setSystemSource("cable");
    CHECK(since(script, mark).empty());
    CHECK(tap.log == Log{"stop", "start:cable"});
    CHECK(s.systemSource() == "cable");

    s.setSystemSource("cable");
    CHECK(tap.log.size() == 2);
}

TEST_CASE("quick source changes while running keep the device", "[session]") {
    Script script;
    TapScript tap;
    Session s(scripted(script), fakeTap(tap));
    s.open(Backend::Null, scriptedConfig("a"));
    rt_snapshot snap{};
    REQUIRE(waitForCallbacks(s, snap, 3));
    const std::size_t mark = script.log.size();
    for (int i = 0; i < 20; ++i) s.setSystemSource(i % 2 == 0 ? "spk" : "cable");
    CHECK(since(script, mark).empty());
    REQUIRE(waitForCallbacks(s, snap, snap.callbacks + 3));
    CHECK(snap.running == 1);
}

TEST_CASE("the source is kept before open and across a reconfigure", "[session]") {
    Script script;
    TapScript tap;
    Session s(scripted(script), fakeTap(tap));
    s.setSystemSource("spk");
    s.open(Backend::Null, scriptedConfig("a"));
    CHECK(tap.log == Log{"start:spk"});

    tap.log.clear();
    CHECK(s.reconfigure(scriptedConfig("b")) == ReconfigureResult::Applied);
    CHECK(tap.log == Log{"stop", "start:spk"});
}

TEST_CASE("stop, then a reconfigure, stops and restarts the tap", "[session]") {
    Script script;
    TapScript tap;
    Session s(scripted(script), fakeTap(tap));
    s.open(Backend::Null, scriptedConfig("a"));
    tap.log.clear();
    s.stop();
    CHECK(tap.log == Log{"stop"});
    CHECK(s.reconfigure(scriptedConfig("a")) == ReconfigureResult::Applied);
    CHECK(tap.log == Log{"stop", "stop", "start:"});
}

TEST_CASE("a tap that fails to start does not stop the device", "[session]") {
    Script script;
    TapScript tap;
    tap.throwOnStart = true;
    Session s(scripted(script), fakeTap(tap));
    s.open(Backend::Null, scriptedConfig("a"));
    rt_snapshot snap{};
    REQUIRE(waitForCallbacks(s, snap, 3));
    CHECK(snap.running == 1);
    CHECK(tap.pulls.load() == 0);
}

TEST_CASE("the snapshot reports the tap", "[session]") {
    Script script;
    TapScript tap;
    tap.status.state = SystemAudioState::Playing;
    tap.status.text  = "Speakers";
    tap.peak         = 0.5f;
    Session s(scripted(script), fakeTap(tap));
    s.open(Backend::Null, scriptedConfig("a"));
    rt_snapshot snap{};
    s.snapshot(snap);
    CHECK(snap.system_state == RT_SYS_PLAYING);
    CHECK(std::string(snap.system_text) == "Speakers");
    CHECK(snap.system_peak == 0.5f);
}

TEST_CASE("without a tap the snapshot says off and there are no sources", "[session]") {
    Script script;
    Session s(scripted(script));
    s.open(Backend::Null, scriptedConfig("a"));
    rt_snapshot snap{};
    s.snapshot(snap);
    CHECK(snap.system_state == RT_SYS_OFF);
    CHECK(s.systemSources().empty());
}

TEST_CASE("system sources come from the tap", "[session]") {
    Script script;
    TapScript tap;
    Session s(scripted(script), fakeTap(tap));
    const std::vector<SystemSource> v = s.systemSources();
    REQUIRE(v.size() == 2);
    CHECK(v[1].id == "cable");
    CHECK(v[1].name == "CABLE Input");
}
