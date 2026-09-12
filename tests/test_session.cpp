#include "ffi/Session.h"
#include "ffi/Utf8.h"
#include "dsp/Biquad.h"
#include "dsp/NoiseGate.h"
#include "dsp/Waveshaper.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
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
