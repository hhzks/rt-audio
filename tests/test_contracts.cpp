#include "core/AudioBufferView.h"
#include "core/ContractHandler.h"
#include "core/ParamInfo.h"
#include "dsp/EffectChain.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

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
