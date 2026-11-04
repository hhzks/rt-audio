#include "core/AudioBufferView.h"
#include "core/ContractHandler.h"
#include "core/ParamInfo.h"
#include "dsp/EffectChain.h"
#include "engine/AudioEngine.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>
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

namespace {

constexpr std::array<ParamInfo, 2> kInfo{{
    {"level", "lvl", "dB", -60.0, 12.0, 0.0, Taper::Linear, 0},
    {"gr",    "gr",  "dB", -26.0,  0.0, 0.0, Taper::Linear, kReadOnly},
}};

class NullEffect final : public IEffect {
public:
    void prepare(double, FrameCount, int) override {}
    void reset() override {}
    void process(AudioBufferView&) noexcept override {}
    const char* name() const noexcept override { return "null"; }
};

class ThrowingEffect final : public IEffect {
public:
    bool throwOnPrepare = false;
    int  processCalls   = 0;
    void prepare(double, FrameCount, int) override {
        if (throwOnPrepare) throw std::runtime_error("prepare failed");
    }
    void reset() override {}
    void process(AudioBufferView&) noexcept override { ++processCalls; }
    const char* name() const noexcept override { return "throwing"; }
};

class LatentEffect final : public IEffect {
public:
    void prepare(double, FrameCount, int) override {}
    void reset() override {}
    void process(AudioBufferView&) noexcept override {}
    const char* name() const noexcept override { return "latent"; }
    FrameCount latencyFrames() const noexcept override { return 4; }
};

bool firstPredicateContains(const char* text) {
    contracts::Violation v;
    return contracts::firstViolation(v) && v.predicate != nullptr &&
           std::strstr(v.predicate, text) != nullptr;
}

} // namespace

#if defined(RT_CONTRACTS_OBSERVE)
#define RT_NEEDS_OBSERVE() ((void)0)
#else
#define RT_NEEDS_OBSERVE() SKIP("Debug builds stop at a failed contract")
#endif

TEST_CASE("met preconditions do not count", "[contracts]") {
    std::array<float, 8> a{}, b{};
    std::array<float*, kMaxChannels> ptrs{a.data(), b.data()};
    AudioBufferView view(ptrs.data(), 2, 8);
    ParamBlock<2> params(kInfo);
    const auto before = contracts::violationCount();
    CHECK(view.channel(1) == b.data());
    CHECK(view.withFrames(8).numFrames() == 8);
    CHECK(params.get(0) == 0.0);
    params.publish(1, -3.0);
    CHECK(params.get(1) == -3.0);
    CHECK(contracts::violationCount() == before);
}

TEST_CASE("a failed channel precondition is counted and recorded", "[contracts]") {
    RT_NEEDS_OBSERVE();
    std::array<float, 8> a{}, b{};
    // kMaxChannels entries, so the read after the counted violation stays inside the array.
    std::array<float*, kMaxChannels> ptrs{a.data(), b.data()};
    AudioBufferView view(ptrs.data(), 2, 8);
    const auto before = contracts::violationCount();
    (void)view.channel(5);
    CHECK(contracts::violationCount() == before + 1);
    if (before == 0) CHECK(firstPredicateContains("c < numChannels_"));
}

TEST_CASE("a failed withFrames precondition is counted", "[contracts]") {
    RT_NEEDS_OBSERVE();
    std::array<float, 8> a{};
    std::array<float*, kMaxChannels> ptrs{a.data()};
    AudioBufferView view(ptrs.data(), 1, 8);
    const auto before = contracts::violationCount();
    (void)view.withFrames(9);
    CHECK(contracts::violationCount() == before + 1);
}

TEST_CASE("publish on a writable param is counted", "[contracts]") {
    RT_NEEDS_OBSERVE();
    ParamBlock<2> params(kInfo);
    const auto before = contracts::violationCount();
    params.publish(0, 1.0);
    CHECK(contracts::violationCount() == before + 1);
}

TEST_CASE("a counted violation does not allocate", "[contracts]") {
    RT_NEEDS_OBSERVE();
    std::array<float, 8> a{};
    std::array<float*, kMaxChannels> ptrs{a.data()};
    AudioBufferView view(ptrs.data(), 1, 8);
    const auto before = contracts::violationCount();
    g_allocations.store(0);
    g_trapArmed.store(true);
    (void)view.channel(3);
    g_trapArmed.store(false);
    CHECK(g_allocations.load() == 0);
    CHECK(contracts::violationCount() == before + 1);
}

TEST_CASE("met chain preconditions do not count", "[contracts]") {
    EffectChain chain;
    chain.add(std::make_unique<NullEffect>());
    const auto before = contracts::violationCount();
    CHECK(chain.at(0) != nullptr);
    CHECK(chain.size() == 1);
    CHECK(contracts::violationCount() == before);
}

TEST_CASE("add with a null effect is counted", "[contracts]") {
    RT_NEEDS_OBSERVE();
    EffectChain chain;
    const auto before = contracts::violationCount();
    chain.add(nullptr);
    CHECK(contracts::violationCount() == before + 1);
}

TEST_CASE("add on a full chain is counted, then throws", "[contracts]") {
    RT_NEEDS_OBSERVE();
    EffectChain chain;
    for (int i = 0; i < kMaxEffects; ++i)
        chain.add(std::make_unique<NullEffect>());
    const auto before = contracts::violationCount();
    CHECK_THROWS_AS(chain.add(std::make_unique<NullEffect>()), std::bad_alloc);
    CHECK(contracts::violationCount() == before + 1);
    CHECK(chain.size() == static_cast<std::size_t>(kMaxEffects));
}

TEST_CASE("a failed prepare leaves the engine unprepared", "[contracts]") {
    AudioEngine engine;
    auto fx = std::make_unique<ThrowingEffect>();
    auto* probe = fx.get();
    engine.chain().add(std::move(fx));
    engine.prepare(48000.0, 16, 1);

    probe->throwOnPrepare = true;
    CHECK_THROWS_AS(engine.prepare(48000.0, 32, 2), std::runtime_error);

    const auto before = contracts::violationCount();
    probe->processCalls = 0;
    std::vector<float> in(64, 0.5f), out(64, 1.0f);
    engine.processInterleaved(in.data(), out.data(), 32);
    CHECK(probe->processCalls == 0);
    CHECK(engine.stats().xruns.load() == 1);
    for (float v : out) CHECK(v == 0.0f);
    CHECK(contracts::violationCount() == before);
}

TEST_CASE("a moved-from chain is empty", "[contracts]") {
    EffectChain a;
    a.add(std::make_unique<LatentEffect>());
    a.add(std::make_unique<LatentEffect>());

    EffectChain b(std::move(a));
    CHECK(b.size() == 2);
    CHECK(b.totalLatencyFrames() == 8);
    REQUIRE(a.size() == 0);
    CHECK(a.totalLatencyFrames() == 0);

    EffectChain c;
    c.add(std::make_unique<NullEffect>());
    c = std::move(b);
    CHECK(c.size() == 2);
    CHECK(c.totalLatencyFrames() == 8);
    REQUIRE(b.size() == 0);

    std::array<float, 8> buf{};
    std::array<float*, kMaxChannels> ptrs{buf.data()};
    AudioBufferView view(ptrs.data(), 1, 8);
    b.process(view);
    a.reset();
}
