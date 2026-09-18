#include "io/asio/AsioDevice.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

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
        *in = 100;
        *out = 200;
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
        for (long i = 0; i < count; ++i) {
            auto& h = halves.emplace_back(static_cast<std::size_t>(2 * size), 0);
            infos[i].buffers[0] = h.data();
            infos[i].buffers[1] = h.data() + size;
            isInput.push_back(infos[i].isInput == ASIOTrue);
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
        panelEntered.set_value();
        if (panelGate.valid()) panelGate.wait();
        return ASE_OK;
    }
    ASIOError future(long, void*) override { return ASE_InvalidParameter; }
    ASIOError outputReady() override { return ASE_NotPresent; }

    DWORD threadOf(const std::string& call) {
        std::lock_guard lock(m);
        return threads[call];
    }
    std::uint64_t blocks() const { return done.load(); }

    bool                     initOk = true;
    std::atomic<ULONG>       refs{1};
    std::atomic<bool>        released{false};
    ASIOSampleRate           rate = 0.0;
    ASIOCallbacks*           callbacks = nullptr;
    long                     frames = 0;
    std::atomic<std::int32_t> lastOutput{0};
    std::promise<void>       panelEntered;
    std::shared_future<void> panelGate;
    std::vector<std::vector<std::int32_t>> halves;

private:
    void record(const char* call) {
        std::lock_guard lock(m);
        threads[call] = GetCurrentThreadId();
    }
    void loop() {
        long half = 0;
        while (running) {
            for (std::size_t b = 0; b < halves.size(); ++b)
                if (isInput[b])
                    std::fill_n(halves[b].data() + half * frames, frames, std::int32_t{1} << 29);
            callbacks->bufferSwitch(half, ASIOTrue);
            for (std::size_t b = 0; b < halves.size(); ++b)
                if (!isInput[b]) lastOutput = halves[b][static_cast<std::size_t>(half * frames)];
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
    REQUIRE(waitFor([&] { return fake.blocks() >= 5; }));
    CHECK(fake.lastOutput.load() == (std::int32_t{1} << 28));   // 0.25 in, 0.125 out
    dev.stop();
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

TEST_CASE("the driver panel opens on the host thread without blocking the caller", "[asio]") {
    FakeAsio fake;
    std::promise<void> release;
    fake.panelGate = release.get_future().share();
    AsioDevice dev(factoryFor(fake));
    Half cb;
    CHECK(!dev.openControlPanel());
    dev.open(fakeConfig(), &cb);
    CHECK(dev.openControlPanel());   // returns while the panel is still open
    fake.panelEntered.get_future().wait();
    CHECK(fake.threadOf("controlPanel") == fake.threadOf("init"));
    release.set_value();
    dev.close();
}
