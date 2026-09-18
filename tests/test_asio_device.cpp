#include "io/asio/AsioDevice.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace rt;
using Catch::Matchers::ContainsSubstring;

namespace {

// A driver with its own callback thread, like a real one. Int32LSB, 64..2048 in powers of two.
class FakeAsio : public IASIO {
public:
    virtual ~FakeAsio() { halt(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        released = true;
        return --refs;
    }

    ASIOBool init(void*) override {
        record("init");
        return initOk ? ASIOTrue : ASIOFalse;
    }
    void getDriverName(char* name) override { std::memcpy(name, "Fake", 5); }
    long getDriverVersion() override { return 1; }
    void getErrorMessage(char* text) override { std::memcpy(text, "fake init failure", 18); }
    ASIOError start() override {
        record("start");
        running = true;
        worker = std::thread([this] { loop(); });
        return ASE_OK;
    }
    ASIOError stop() override {
        record("stop");
        halt();
        return ASE_OK;
    }
    ASIOError getChannels(long* in, long* out) override {
        *in = 2;
        *out = 2;
        return ASE_OK;
    }
    ASIOError getLatencies(long* in, long* out) override {
        *in = inLatency;
        *out = outLatency;
        return ASE_OK;
    }
    ASIOError getBufferSize(long* minSize, long* maxSize, long* preferred, long* granularity) override {
        *minSize = 64;
        *maxSize = 2048;
        *preferred = 256;
        *granularity = -1;
        return ASE_OK;
    }
    ASIOError canSampleRate(ASIOSampleRate r) override {
        return r == 44100.0 || r == 48000.0 ? ASE_OK : ASE_NoClock;
    }
    ASIOError getSampleRate(ASIOSampleRate* r) override {
        *r = rate;
        return ASE_OK;
    }
    ASIOError setSampleRate(ASIOSampleRate r) override {
        if (setRateFails) return ASE_NoClock;
        rate = r;
        return ASE_OK;
    }
    ASIOError getClockSources(ASIOClockSource*, long* n) override {
        *n = 0;
        return ASE_OK;
    }
    ASIOError setClockSource(long) override { return ASE_OK; }
    ASIOError getSamplePosition(ASIOSamples*, ASIOTimeStamp*) override { return ASE_NotPresent; }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        info->type = ASIOSTInt32LSB;
        std::memcpy(info->name, "ch", 3);
        return ASE_OK;
    }
    ASIOError createBuffers(ASIOBufferInfo* infos, long count, long size, ASIOCallbacks* cb) override {
        record("createBuffers");
        callbacks = cb;
        frames = size;
        halves.clear();
        halves.reserve(static_cast<std::size_t>(count));
        isInput.clear();
        channels.clear();
        for (long i = 0; i < count; ++i) {
            auto& h = halves.emplace_back(static_cast<std::size_t>(2 * size), 0);
            infos[i].buffers[0] = h.data();
            infos[i].buffers[1] = h.data() + size;
            isInput.push_back(infos[i].isInput == ASIOTrue);
            channels.push_back(infos[i].channelNum);
        }
        return ASE_OK;
    }
    ASIOError disposeBuffers() override {
        record("disposeBuffers");
        halves.clear();
        return ASE_OK;
    }
    ASIOError controlPanel() override {
        record("controlPanel");
        if (++panelCalls == 1) panelEntered.set_value();
        if (panelGate.valid()) panelGate.wait();
        return panelResult;
    }
    ASIOError future(long, void*) override { return ASE_InvalidParameter; }
    ASIOError outputReady() override { return ASE_NotPresent; }

    DWORD threadOf(const std::string& call) {
        std::lock_guard lock(m);
        return threads[call];
    }
    std::uint64_t blocks() const { return done.load(); }

    bool                       initOk = true;
    bool                       setRateFails = false;
    std::atomic<ULONG>         refs{1};
    std::atomic<bool>          released{false};
    ASIOSampleRate             rate = 0.0;
    std::atomic<long>          inLatency{100}, outLatency{200};
    ASIOCallbacks*             callbacks = nullptr;
    long                       frames = 0;
    std::atomic<std::uint64_t> mismatches{0};   // output blocks that are not their input x 0.5
    std::promise<void>         panelEntered;
    std::shared_future<void>   panelGate;
    std::atomic<int>           panelCalls{0};
    ASIOError                  panelResult = ASE_OK;
    std::vector<std::vector<std::int32_t>> halves;

private:
    static std::int32_t inputValue(std::uint64_t block, long channel) {
        return static_cast<std::int32_t>(block % 1000 + 1 + static_cast<std::uint64_t>(channel)) << 20;
    }
    void record(const char* call) {
        std::lock_guard lock(m);
        threads[call] = GetCurrentThreadId();
    }
    void loop() {
        long half = 0;
        for (std::uint64_t block = 0; running; ++block) {
            for (std::size_t b = 0; b < halves.size(); ++b)
                std::fill_n(halves[b].data() + half * frames, frames,
                            isInput[b] ? inputValue(block, channels[b]) : 0);
            if (block % 2 == 1) {
                ASIOTime timeInfo{};
                callbacks->bufferSwitchTimeInfo(&timeInfo, half, ASIOTrue);
            } else {
                callbacks->bufferSwitch(half, ASIOTrue);
            }
            for (std::size_t b = 0; b < halves.size(); ++b) {
                if (isInput[b]) continue;
                const std::int32_t want = inputValue(block, channels[b]) / 2;
                const std::int32_t* out = halves[b].data() + half * frames;
                if (!std::all_of(out, out + frames, [want](std::int32_t v) { return v == want; }))
                    ++mismatches;
            }
            ++done;
            half ^= 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void halt() {
        running = false;
        if (worker.joinable()) worker.join();
    }

    std::vector<bool>          isInput;
    std::vector<long>          channels;
    std::atomic<bool>          running{false};
    std::thread                worker;
    std::atomic<std::uint64_t> done{0};
    std::mutex                 m;
    std::map<std::string, DWORD> threads;
};

struct Half : IAudioCallback {
    void audioDeviceProcess(const float* in, float* out, FrameCount frames) noexcept override {
        for (std::size_t i = 0; i < idx(frames) * 2; ++i) out[i] = in[i] * 0.5f;
    }
};

DeviceConfig fakeConfig(FrameCount block = 100) {
    DeviceConfig c;
    c.inputId = c.outputId = "Fake";
    c.sampleRate  = 48000.0;
    c.blockFrames = block;
    return c;
}

AsioDevice::DriverFactory factoryFor(FakeAsio& fake) {
    return [&fake](const std::string& id) -> IASIO* {
        if (id != "Fake") throw std::runtime_error("unexpected id " + id);
        fake.AddRef();
        return &fake;
    };
}

template <class Pred>
bool waitFor(Pred done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

} // namespace

TEST_CASE("open sets the rate, picks the buffer size and fills the status", "[asio]") {
    FakeAsio fake;
    AsioDevice dev(factoryFor(fake));
    Half cb;
    dev.open(fakeConfig(100), &cb);
    CHECK(fake.rate == 48000.0);
    CHECK(fake.frames == 128);
    CHECK(fake.halves.size() == 4);
    const DeviceStatus st = dev.status();
    CHECK(st.blockFrames == 128);
    CHECK(st.sampleRate == 48000.0);
    CHECK(st.estimatedRoundTripMs == 1000.0 * 300.0 / 48000.0);
    CHECK_THAT(st.backendName, ContainsSubstring("Fake") && ContainsSubstring("Int32LSB"));
    dev.close();
    CHECK(fake.released);
}

TEST_CASE("audio passes through the engine callback", "[asio]") {
    FakeAsio fake;
    AsioDevice dev(factoryFor(fake));
    Half cb;
    dev.open(fakeConfig(), &cb);
    dev.start();
    REQUIRE(waitFor([&] { return fake.blocks() >= 6; }));
    dev.stop();
    CHECK(fake.mismatches.load() == 0);
}

TEST_CASE("the host thread makes every driver call", "[asio]") {
    FakeAsio fake;
    AsioDevice dev(factoryFor(fake));
    Half cb;
    dev.open(fakeConfig(), &cb);
    dev.start();
    dev.stop();
    dev.close();
    const DWORD host = fake.threadOf("init");
    CHECK(host != 0);
    CHECK(host != GetCurrentThreadId());
    CHECK(fake.threadOf("createBuffers") == host);
    CHECK(fake.threadOf("start") == host);
    CHECK(fake.threadOf("stop") == host);
    CHECK(fake.threadOf("disposeBuffers") == host);
}

TEST_CASE("a reset request stops the device with a reason", "[asio]") {
    FakeAsio fake;
    AsioDevice dev(factoryFor(fake));
    Half cb;
    dev.open(fakeConfig(), &cb);
    dev.start();
    REQUIRE(dev.isRunning());
    CHECK(fake.callbacks->asioMessage(kAsioResetRequest, 0, nullptr, nullptr) == 1);
    REQUIRE(waitFor([&] { return !dev.isRunning(); }));
    CHECK(dev.status().lastError == "the ASIO driver requested a reset");
    CHECK(fake.released);
}

TEST_CASE("a latency change updates the round trip", "[asio]") {
    FakeAsio fake;
    AsioDevice dev(factoryFor(fake));
    Half cb;
    dev.open(fakeConfig(), &cb);
    fake.inLatency  = 300;
    fake.outLatency = 420;
    CHECK(fake.callbacks->asioMessage(kAsioLatenciesChanged, 0, nullptr, nullptr) == 1);
    CHECK(waitFor([&] { return dev.status().estimatedRoundTripMs == 1000.0 * 720.0 / 48000.0; }));
}

TEST_CASE("a rate callback resets only for a rate that was not requested", "[asio]") {
    FakeAsio fake;
    AsioDevice dev(factoryFor(fake));
    Half cb;
    dev.open(fakeConfig(), &cb);
    fake.callbacks->sampleRateDidChange(48000.0);
    CHECK_NOTHROW(dev.start());
    CHECK(dev.isRunning());
    fake.callbacks->sampleRateDidChange(44100.0);
    REQUIRE(waitFor([&] { return !dev.isRunning(); }));
    CHECK(dev.status().lastError == "the ASIO driver requested a reset");
    CHECK(fake.released);
}

TEST_CASE("start after a reset gives the reset as the reason", "[asio]") {
    FakeAsio fake;
    AsioDevice dev(factoryFor(fake));
    Half cb;
    dev.open(fakeConfig(), &cb);
    CHECK(fake.callbacks->asioMessage(kAsioResetRequest, 0, nullptr, nullptr) == 1);
    CHECK_THROWS_WITH(dev.start(), "the ASIO driver requested a reset");
}

TEST_CASE("a resync request counts an xrun", "[asio]") {
    FakeAsio fake;
    AsioDevice dev(factoryFor(fake));
    Half cb;
    dev.open(fakeConfig(), &cb);
    CHECK(fake.callbacks->asioMessage(kAsioResyncRequest, 0, nullptr, nullptr) == 1);
    CHECK(dev.status().xruns == 1);
}

TEST_CASE("two different driver ids are rejected", "[asio]") {
    FakeAsio fake;
    AsioDevice dev(factoryFor(fake));
    Half cb;
    DeviceConfig c = fakeConfig();
    c.outputId = "Other";
    CHECK_THROWS_AS(dev.open(c, &cb), std::invalid_argument);
}

TEST_CASE("an init failure reports the driver message and frees the process slot", "[asio]") {
    FakeAsio bad;
    bad.initOk = false;
    {
        AsioDevice dev(factoryFor(bad));
        Half cb;
        CHECK_THROWS_WITH(dev.open(fakeConfig(), &cb), ContainsSubstring("fake init failure"));
        CHECK(bad.released);
    }
    FakeAsio good;
    AsioDevice dev(factoryFor(good));
    Half cb;
    CHECK_NOTHROW(dev.open(fakeConfig(), &cb));
}

TEST_CASE("an unsupported rate names the rate", "[asio]") {
    FakeAsio fake;
    AsioDevice dev(factoryFor(fake));
    Half cb;
    DeviceConfig c = fakeConfig();
    c.sampleRate = 96000.0;
    CHECK_THROWS_WITH(dev.open(c, &cb), ContainsSubstring("96000"));
}

TEST_CASE("a failed rate change names the rate", "[asio]") {
    FakeAsio fake;
    fake.setRateFails = true;
    AsioDevice dev(factoryFor(fake));
    Half cb;
    CHECK_THROWS_WITH(dev.open(fakeConfig(), &cb), ContainsSubstring("48000"));
    CHECK(fake.released);
}

TEST_CASE("a second open device in the process is refused", "[asio]") {
    FakeAsio a, b;
    AsioDevice first(factoryFor(a));
    AsioDevice second(factoryFor(b));
    Half cb;
    first.open(fakeConfig(), &cb);
    CHECK_THROWS_WITH(second.open(fakeConfig(), &cb), ContainsSubstring("another ASIO device"));
    first.close();
    CHECK_NOTHROW(second.open(fakeConfig(), &cb));
}

TEST_CASE("a modal driver panel gives Modal, and a second call is not queued", "[asio]") {
    FakeAsio fake;
    std::promise<void> release;
    fake.panelGate = release.get_future().share();
    AsioDevice dev(factoryFor(fake));
    Half cb;
    dev.open(fakeConfig(), &cb);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(dev.openControlPanel(fakeConfig()) == PanelResult::Modal);
    CHECK(std::chrono::steady_clock::now() - t0 >= AsioDevice::kPanelWait);
    CHECK(dev.panelOpen());
    CHECK(dev.openControlPanel(fakeConfig()) == PanelResult::AlreadyOpen);
    CHECK(fake.threadOf("controlPanel") == fake.threadOf("init"));
    release.set_value();
    CHECK(waitFor([&] { return !dev.panelOpen(); }));
    CHECK(fake.panelCalls == 1);
    dev.close();
}

TEST_CASE("a modeless driver panel gives Opened", "[asio]") {
    FakeAsio fake;
    AsioDevice dev(factoryFor(fake));
    Half cb;
    dev.open(fakeConfig(), &cb);
    CHECK(dev.openControlPanel(fakeConfig()) == PanelResult::Opened);
    CHECK(!dev.panelOpen());
}

TEST_CASE("a driver without a panel gives NoDriverPanel, and other errors throw", "[asio]") {
    FakeAsio fake;
    AsioDevice dev(factoryFor(fake));
    Half cb;
    dev.open(fakeConfig(), &cb);
    fake.panelResult = ASE_NotPresent;
    CHECK(dev.openControlPanel(fakeConfig()) == PanelResult::NoDriverPanel);
    fake.panelResult = ASE_HWMalfunction;
    CHECK_THROWS_WITH(dev.openControlPanel(fakeConfig()), ContainsSubstring("did not open"));
}

TEST_CASE("the panel on a device that is not open loads the driver only", "[asio]") {
    FakeAsio fake;
    AsioDevice dev(factoryFor(fake));
    CHECK(dev.openControlPanel(fakeConfig()) == PanelResult::Opened);
    CHECK(fake.threadOf("init") != 0);
    CHECK(fake.threadOf("controlPanel") == fake.threadOf("init"));
    CHECK(fake.threadOf("createBuffers") == 0);
    CHECK(fake.callbacks == nullptr);
    CHECK(!fake.released);
    CHECK(!dev.isRunning());
    dev.close();
    CHECK(fake.released);
}

TEST_CASE("open after a panel-only load releases that driver first", "[asio]") {
    FakeAsio fake;
    AsioDevice dev(factoryFor(fake));
    Half cb;
    REQUIRE(dev.openControlPanel(fakeConfig()) == PanelResult::Opened);
    dev.open(fakeConfig(), &cb);
    CHECK(fake.released);
    CHECK(fake.threadOf("createBuffers") != 0);
    dev.start();
    CHECK(dev.isRunning());
}
