#include "core/AtomicPeak.h"
#include "engine/AudioEngine.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <atomic>
#include <vector>

using namespace rt;
using Catch::Matchers::WithinAbs;

namespace {

// Reproduces one interleaving: the consumer's exchange(0) lands between the
// producer's load and its compare-exchange.
struct RacingAtomic {
    float value = 0.9f;
    bool  raced = false;

    float load(std::memory_order) const noexcept { return value; }

    bool compare_exchange_weak(float& expected, float desired, std::memory_order) noexcept {
        if (!raced) { raced = true; value = 0.0f; }
        if (value == expected) { value = desired; return true; }
        expected = value;
        return false;
    }
};

} // namespace

TEST_CASE("raisePeak keeps the maximum", "[core]") {
    std::atomic<float> a{0.2f};
    raisePeak(a, 0.7f);
    CHECK(a.load() == 0.7f);
    raisePeak(a, 0.3f);
    CHECK(a.load() == 0.7f);
}

// The stale-compare loop `while (v > prev && !cas(...))` fails this test: it
// sees prev = 0.9, skips the store, and the 0.5 peak is lost.
TEST_CASE("raisePeak survives a concurrent take", "[core]") {
    RacingAtomic a;
    raisePeak(a, 0.5f);
    CHECK(a.raced);
    CHECK(a.value == 0.5f);
}

TEST_CASE("per-channel peaks are independent", "[engine]") {
    AudioEngine engine;
    engine.prepare(48000.0, 64, 2);
    std::vector<float> in(64 * 2), out(64 * 2);
    for (int i = 0; i < 64; ++i) { in[idx(i * 2)] = 0.8f; in[idx(i * 2 + 1)] = 0.1f; }
    engine.processInterleaved(in.data(), out.data(), 64);

    auto& s = engine.stats();
    CHECK_THAT(s.inputPeak[0].exchange(0.0f),  WithinAbs(0.8, 1e-6));
    CHECK_THAT(s.inputPeak[1].exchange(0.0f),  WithinAbs(0.1, 1e-6));
    CHECK_THAT(s.outputPeak[0].exchange(0.0f), WithinAbs(0.8, 1e-6));
    CHECK_THAT(s.outputPeak[1].exchange(0.0f), WithinAbs(0.1, 1e-6));
}

TEST_CASE("a take leaves zero until the next block", "[engine]") {
    AudioEngine engine;
    engine.prepare(48000.0, 64, 1);
    std::vector<float> in(64, 0.4f), out(64);
    engine.processInterleaved(in.data(), out.data(), 64);

    auto& s = engine.stats();
    CHECK_THAT(s.outputPeak[0].exchange(0.0f), WithinAbs(0.4, 1e-6));
    CHECK_THAT(s.outputPeak[0].exchange(0.0f), WithinAbs(0.0, 1e-9));
    engine.processInterleaved(in.data(), out.data(), 64);
    CHECK_THAT(s.outputPeak[0].exchange(0.0f), WithinAbs(0.4, 1e-6));
}

TEST_CASE("clip counters are monotonic", "[engine]") {
    AudioEngine engine;
    engine.prepare(48000.0, 64, 1);
    CHECK(engine.stats().inputClips.load() == 0);
    CHECK(engine.stats().outputClips.load() == 0);

    std::vector<float> in(64, 0.5f), out(64);
    in[3] = 1.0f;    // input at full scale: counted; output exactly 1.0: not clamped
    in[7] = -1.2f;   // input over full scale: counted; output clamped: counted
    engine.processInterleaved(in.data(), out.data(), 64);
    CHECK(engine.stats().inputClips.load() == 2);
    CHECK(engine.stats().outputClips.load() == 1);

    engine.processInterleaved(in.data(), out.data(), 64);
    CHECK(engine.stats().inputClips.load() == 4);
    CHECK(engine.stats().outputClips.load() == 2);
}

TEST_CASE("monitor records meters and the callback", "[engine]") {
    AudioEngine engine;
    engine.prepare(48000.0, 64, 2);
    std::vector<float> in(64 * 2, 0.0f), out(64 * 2, 0.0f);
    for (int i = 0; i < 64; ++i) { in[idx(i * 2)] = 0.8f; out[idx(i * 2 + 1)] = 0.3f; }
    in[5]  = -1.0f;   // channel 1 at full scale: an input clip
    out[8] = 1.5f;    // channel 0 over full scale: an output clip
    const std::vector<float> before = out;
    engine.monitor(in.data(), out.data(), 64);

    auto& s = engine.stats();
    CHECK(s.callbackCount.load() == 1);
    CHECK_THAT(s.inputPeak[0].exchange(0.0f),  WithinAbs(0.8, 1e-6));
    CHECK_THAT(s.inputPeak[1].exchange(0.0f),  WithinAbs(1.0, 1e-6));
    CHECK_THAT(s.outputPeak[0].exchange(0.0f), WithinAbs(1.5, 1e-6));
    CHECK_THAT(s.outputPeak[1].exchange(0.0f), WithinAbs(0.3, 1e-6));
    CHECK(s.inputClips.load() == 1);
    CHECK(s.outputClips.load() == 1);
    CHECK(out == before);
}

TEST_CASE("monitor on an oversized block records an xrun", "[engine]") {
    AudioEngine engine;
    engine.prepare(48000.0, 64, 1);
    std::vector<float> in(128, 0.5f), out(128, 0.5f);
    engine.monitor(in.data(), out.data(), 128);
    CHECK(engine.stats().xruns.load() == 1);
    CHECK(engine.stats().callbackCount.load() == 0);
}
