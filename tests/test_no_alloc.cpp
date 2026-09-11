// THE MOST IMPORTANT TEST IN THIS PROJECT.
//
// C++ has no way to enforce realtime safety at compile time, so this runtime
// trap is the closest substitute.

#include "engine/AudioEngine.h"
#include "engine/LoopbackProbe.h"
#include "dsp/Biquad.h"
#include "dsp/NoiseGate.h"
#include "dsp/Waveshaper.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

namespace {
std::atomic<bool> g_trapArmed{false};
std::atomic<int>  g_allocations{0};
}

void* operator new(std::size_t n) {
    if (g_trapArmed.load(std::memory_order_relaxed))
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void  operator delete(void* p)                noexcept { std::free(p); }
void  operator delete[](void* p)              noexcept { std::free(p); }
void  operator delete(void* p, std::size_t)   noexcept { std::free(p); }
void  operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace rt;
using Catch::Matchers::WithinAbs;

TEST_CASE("engine block does not allocate", "[engine]") {
    AudioEngine engine;
    engine.chain().add(std::make_unique<Biquad>(Biquad::Type::HighPass, 80.0, 0.707));
    engine.chain().add(std::make_unique<NoiseGate>(engine.params()));
    engine.chain().add(std::make_unique<Waveshaper>(engine.params()));
    engine.chain().add(std::make_unique<Biquad>(Biquad::Type::Peak, 3000.0, 1.0, 3.0));

    constexpr FrameCount kBlock = 256;
    constexpr int        kCh    = 2;
    engine.prepare(48000.0, kBlock, kCh);   // allocation is allowed HERE

    // Make sure parameters are non-trivial so no branch is skipped.
    engine.params().drive.store(4.0f);
    engine.params().mix.store(0.8f);
    engine.params().gateThresholdDb.store(-30.0f);

    std::vector<float> in(kBlock * kCh), out(kBlock * kCh);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = 0.5f * std::sin(0.05f * static_cast<float>(i));

    g_allocations = 0;
    g_trapArmed = true;
    for (int i = 0; i < 100; ++i)
        engine.processInterleaved(in.data(), out.data(), kBlock);
    g_trapArmed = false;

    CHECK(g_allocations.load() == 0);
}

TEST_CASE("bypass path does not allocate", "[engine]") {
    AudioEngine engine;
    engine.chain().add(std::make_unique<Waveshaper>(engine.params()));
    engine.prepare(48000.0, 128, 2);
    engine.params().bypass.store(true);

    std::vector<float> in(128 * 2, 0.1f), out(128 * 2);
    g_allocations = 0;
    g_trapArmed = true;
    for (int i = 0; i < 50; ++i) engine.processInterleaved(in.data(), out.data(), 128);
    g_trapArmed = false;
    CHECK(g_allocations.load() == 0);
}

// An oversized block must fail safe (silence + xrun), not read out of bounds.
TEST_CASE("oversized block is safe", "[engine]") {
    AudioEngine engine;
    engine.prepare(48000.0, 64, 2);
    std::vector<float> in(512 * 2, 1.0f), out(512 * 2, 7.0f);
    engine.processInterleaved(in.data(), out.data(), 128);   // bigger than prepared
    CHECK(engine.stats().xruns.load() > 0);
    CHECK_THAT(out[0], WithinAbs(0.0, 1e-9));
}

TEST_CASE("loopback probe does not allocate", "[engine]") {
    SweepConfig cfg;
    cfg.seconds = 0.05; cfg.primeSeconds = 0.01;
    cfg.maxLatencySeconds = 0.02; cfg.tailSeconds = 0.01;

    LoopbackProbe probe;
    probe.prepare(48000.0, 128, 2, cfg);
    probe.arm();

    std::vector<float> in(128 * 2, 0.1f), out(128 * 2);
    g_allocations = 0;
    g_trapArmed = true;
    for (int i = 0; i < 100; ++i) probe.process(in.data(), out.data(), 128);
    g_trapArmed = false;
    CHECK(g_allocations.load() == 0);
}
