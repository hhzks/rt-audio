#include "core/SpscRingBuffer.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

using namespace rt;
using Catch::Matchers::WithinAbs;

TEST_CASE("basic push and pop", "[core]") {
    SpscRingBuffer ring;
    ring.reset(16);
    CHECK(ring.capacity() >= 16);
    CHECK(ring.readAvailable() == 0);

    const float in[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    CHECK(ring.push(in, 4) == 4);
    CHECK(ring.readAvailable() == 4);

    float out[4] = {};
    CHECK(ring.pop(out, 4) == 4);
    for (int i = 0; i < 4; ++i) CHECK_THAT(out[i], WithinAbs(static_cast<double>(in[i]), 1e-9));
    CHECK(ring.readAvailable() == 0);
}

TEST_CASE("indices wrap around", "[core]") {
    SpscRingBuffer ring;
    ring.reset(8);
    float v = 0.0f, out[3] = {};
    // Push/pop repeatedly so indices wrap many times over.
    for (int cycle = 0; cycle < 100; ++cycle) {
        const float in[3] = { v, v + 1, v + 2 };
        CHECK(ring.push(in, 3) == 3);
        CHECK(ring.pop(out, 3) == 3);
        for (int i = 0; i < 3; ++i)
            CHECK_THAT(out[i], WithinAbs(static_cast<double>(v + static_cast<float>(i)), 1e-9));
        v += 3.0f;
    }
}

TEST_CASE("overflow is clamped", "[core]") {
    SpscRingBuffer ring;
    ring.reset(8);                       // capacity 8, usable 7
    std::vector<float> big(100, 1.0f);
    const auto pushed = ring.push(big.data(), big.size());
    CHECK(pushed < big.size());          // must refuse, never overwrite
    CHECK(ring.readAvailable() == pushed);
}

TEST_CASE("popOrZero signals underrun", "[core]") {
    SpscRingBuffer ring;
    ring.reset(16);
    const float in[2] = { 5.0f, 6.0f };
    ring.push(in, 2);

    float out[4] = { 9, 9, 9, 9 };
    CHECK(ring.popOrZero(out, 4) == true);   // underrun reported
    CHECK_THAT(out[0], WithinAbs(5.0, 1e-9));
    CHECK_THAT(out[1], WithinAbs(6.0, 1e-9));
    CHECK_THAT(out[2], WithinAbs(0.0, 1e-9));   // shortfall zero-filled
    CHECK_THAT(out[3], WithinAbs(0.0, 1e-9));
}

TEST_CASE("pushSilence writes zeros up to the free space", "[core]") {
    SpscRingBuffer ring;
    ring.reset(8);                       // capacity 8, usable 7
    const float ones[7] = { 1, 1, 1, 1, 1, 1, 1 };
    float sink[7] = {};
    ring.push(ones, 7);
    ring.pop(sink, 7);                   // every slot now holds 1.0

    CHECK(ring.pushSilence(5) == 5);
    CHECK(ring.pushSilence(5) == 2);     // must refuse, never overwrite
    CHECK(ring.readAvailable() == 7);

    float out[7] = { 9, 9, 9, 9, 9, 9, 9 };
    CHECK(ring.pop(out, 7) == 7);
    for (int i = 0; i < 7; ++i) CHECK_THAT(out[i], WithinAbs(0.0, 0.0));
}

TEST_CASE("discard drops the oldest samples", "[core]") {
    SpscRingBuffer ring;
    ring.reset(16);

    // Dropping from an empty ring is a no-op, not an underflow of readIdx_.
    ring.discard(4);
    CHECK(ring.readAvailable() == 0);

    const float in[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    CHECK(ring.push(in, 8) == 8);

    // Drop the three oldest; whatever remains must still be in order.
    ring.discard(3);
    CHECK(ring.readAvailable() == 5);

    float out[5] = {};
    CHECK(ring.pop(out, 5) == 5);
    for (int i = 0; i < 5; ++i) CHECK_THAT(out[i], WithinAbs(static_cast<double>(in[i + 3]), 1e-9));

    // Over-asking clamps to what is there rather than running the read index
    // past the write index.
    CHECK(ring.push(in, 4) == 4);
    ring.discard(100);
    CHECK(ring.readAvailable() == 0);
    CHECK(ring.writeAvailable() == ring.capacity() - 1);
}

// The property that actually matters: no lost or duplicated samples when
// producer and consumer run concurrently on different cores.
TEST_CASE("concurrent producer and consumer lose nothing", "[core]") {
    SpscRingBuffer ring;
    ring.reset(1024);

    constexpr int kTotal = 200000;
    std::atomic<bool> producerDone{false};

    std::thread producer([&] {
        float next = 0.0f;
        int written = 0;
        while (written < kTotal) {
            float chunk[64];
            const int n = std::min(64, kTotal - written);
            for (int i = 0; i < n; ++i) chunk[i] = next + static_cast<float>(i);
            const auto pushed = ring.push(chunk, static_cast<std::size_t>(n));
            next    += static_cast<float>(pushed);
            written += static_cast<int>(pushed);
            if (pushed == 0) std::this_thread::yield();
        }
        producerDone = true;
    });

    int read = 0;
    float expected = 0.0f;
    bool sequenceOk = true;
    while (read < kTotal) {
        float chunk[128];
        const auto got = ring.pop(chunk, 128);
        for (std::size_t i = 0; i < got; ++i) {
            if (std::fabs(chunk[i] - expected) > 1e-3f) sequenceOk = false;
            expected += 1.0f;
        }
        read += static_cast<int>(got);
        if (got == 0 && !producerDone.load()) std::this_thread::yield();
    }
    producer.join();

    CHECK(sequenceOk);
    CHECK(read == kTotal);
}
