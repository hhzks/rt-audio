#include "core/RingPush.h"
#include "core/DriftController.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

using namespace rt;
using Catch::Matchers::WithinAbs;

namespace {

constexpr std::size_t kCh = 2;

// channel 0 positive, channel 1 negative, so a split interleave is visible.
std::vector<float> makeBlock(std::size_t frames, float tag = 0.0f) {
    std::vector<float> b(frames * kCh);
    for (std::size_t f = 0; f < frames; ++f) {
        b[f * kCh]     =  (static_cast<float>(f) + 1.0f) + tag;
        b[f * kCh + 1] = -(static_cast<float>(f) + 1.0f) - tag;
    }
    return b;
}

bool interleaveIntact(const std::vector<float>& v) {
    for (std::size_t i = 0; i + 1 < v.size(); i += kCh)
        if (!(v[i] > 0.0f && v[i + 1] < 0.0f)) return false;
    return true;
}

std::vector<float> drain(SpscRingBuffer& ring) {
    std::vector<float> out(ring.readAvailable());
    if (!out.empty()) ring.pop(out.data(), out.size());
    return out;
}

} // namespace

TEST_CASE("empty ring takes exactly the block", "[core]") {
    SpscRingBuffer ring;
    ring.reset(192 * kCh * 4);

    const auto block = makeBlock(192);
    const bool evicted = pushEvictingOldest(ring, block.data(), block.size(), kCh);

    CHECK_FALSE(evicted);
    CHECK(ring.readAvailable() == block.size());
    CHECK(drain(ring) == block);
}

TEST_CASE("repeated blocks with room never evict", "[core]") {
    SpscRingBuffer ring;
    ring.reset(192 * kCh * 4);

    for (int i = 0; i < 3; ++i) {
        CAPTURE(i);
        const auto b = makeBlock(192, static_cast<float>(i) * 1000.0f);
        CHECK_FALSE(pushEvictingOldest(ring, b.data(), b.size(), kCh));
    }
    CHECK(ring.readAvailable() == 192 * kCh * 3);
    CHECK(interleaveIntact(drain(ring)));
}

TEST_CASE("overflow evicts and keeps frame alignment", "[core]") {
    SpscRingBuffer ring;
    ring.reset(192 * kCh * 4);

    bool sawEviction = false;
    for (int i = 0; i < 6; ++i) {
        const auto b = makeBlock(192, static_cast<float>(i) * 1000.0f);
        sawEviction |= pushEvictingOldest(ring, b.data(), b.size(), kCh);
    }

    CHECK(sawEviction);
    CHECK(ring.readAvailable() % kCh == 0);
    CHECK(interleaveIntact(drain(ring)));
}

TEST_CASE("oversized block keeps the newest frames", "[core]") {
    SpscRingBuffer ring;
    ring.reset(64);

    const auto block = makeBlock(128);
    pushEvictingOldest(ring, block.data(), block.size(), kCh);

    CHECK(ring.readAvailable() % kCh == 0);
    CHECK(ring.readAvailable() < ring.capacity());

    const auto out = drain(ring);
    CHECK(interleaveIntact(out));
    if (out.size() >= kCh)
        CHECK_THAT(out[out.size() - kCh],
                   WithinAbs(static_cast<double>(block[block.size() - kCh]), 0.0));
}

TEST_CASE("never exceeds capacity", "[core]") {
    SpscRingBuffer ring;
    ring.reset(192 * kCh * 4);

    for (int i = 0; i < 20; ++i) {
        CAPTURE(i);
        const auto b = makeBlock(192, static_cast<float>(i));
        pushEvictingOldest(ring, b.data(), b.size(), kCh);
        CHECK(ring.readAvailable() < ring.capacity());
    }
}

TEST_CASE("priming replaces stale samples with silence up to the target", "[core]") {
    SpscRingBuffer ring;
    ring.reset(192 * kCh * 4);
    const auto stale = makeBlock(100);
    ring.push(stale.data(), stale.size());

    primeRing(ring, 256, kCh);

    CHECK(ring.readAvailable() == 256 * kCh);
    const auto out = drain(ring);
    CHECK(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("a primed ring starts the drift loop at zero error", "[core]") {
    constexpr std::size_t kFrames = 144, kTargetFrames = 288;
    SpscRingBuffer ring;
    ring.reset(kFrames * kCh * 4);
    DriftController drift;

    primeRing(ring, kTargetFrames, kCh);
    drift.prepare(kTargetFrames * kCh, 0.002);

    const auto block = makeBlock(kFrames);
    std::vector<float> out(block.size());
    double worst = 0.0;
    for (int i = 0; i < 2000; ++i) {
        worst = std::max(worst, std::fabs(drift.update(ring.readAvailable()) - 1.0));
        pushEvictingOldest(ring, block.data(), block.size(), kCh);
        ring.popOrZero(out.data(), out.size());
    }
    CAPTURE(worst);
    CHECK(worst < 1e-12);
}

TEST_CASE("render leaves a primed ring alone until the first capture block", "[core]") {
    constexpr std::size_t kFrames = 144, kTargetFrames = 512;
    SpscRingBuffer ring;
    ring.reset(kFrames * kCh * 4);
    DriftController drift;

    primeRing(ring, kTargetFrames, kCh);
    drift.prepare(kTargetFrames * kCh, 0.002);

    const auto block = makeBlock(kFrames);
    std::vector<float> out(block.size(), 1.0f);
    for (int i = 0; i < 3; ++i) popForRender(ring, out.data(), out.size(), false);

    CHECK(ring.readAvailable() == kTargetFrames * kCh);
    CHECK(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; }));

    double worst = 0.0;
    for (int i = 0; i < 2000; ++i) {
        worst = std::max(worst, std::fabs(drift.update(ring.readAvailable()) - 1.0));
        pushEvictingOldest(ring, block.data(), block.size(), kCh);
        popForRender(ring, out.data(), out.size(), true);
    }
    CAPTURE(worst);
    CHECK(worst < 1e-12);
}
