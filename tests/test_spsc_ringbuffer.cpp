#include "core/SpscRingBuffer.h"
#include "TestHarness.h"
#include <atomic>
#include <thread>
#include <vector>

using namespace rt;

void testBasicPushPop() {
    SpscRingBuffer ring;
    ring.reset(16);
    CHECK(ring.capacity() >= 16);
    CHECK(ring.readAvailable() == 0);

    const float in[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    CHECK(ring.push(in, 4) == 4);
    CHECK(ring.readAvailable() == 4);

    float out[4] = {};
    CHECK(ring.pop(out, 4) == 4);
    for (int i = 0; i < 4; ++i) CHECK_NEAR(out[i], in[i], 1e-9);
    CHECK(ring.readAvailable() == 0);
}

void testWrapAround() {
    SpscRingBuffer ring;
    ring.reset(8);
    float v = 0.0f, out[3] = {};
    // Push/pop repeatedly so indices wrap many times over.
    for (int cycle = 0; cycle < 100; ++cycle) {
        const float in[3] = { v, v + 1, v + 2 };
        CHECK(ring.push(in, 3) == 3);
        CHECK(ring.pop(out, 3) == 3);
        for (int i = 0; i < 3; ++i) CHECK_NEAR(out[i], v + static_cast<float>(i), 1e-9);
        v += 3.0f;
    }
}

void testOverflowIsClamped() {
    SpscRingBuffer ring;
    ring.reset(8);                       // capacity 8, usable 7
    std::vector<float> big(100, 1.0f);
    const auto pushed = ring.push(big.data(), big.size());
    CHECK(pushed < big.size());          // must refuse, never overwrite
    CHECK(ring.readAvailable() == pushed);
}

void testPopOrZeroSignalsUnderrun() {
    SpscRingBuffer ring;
    ring.reset(16);
    const float in[2] = { 5.0f, 6.0f };
    ring.push(in, 2);

    float out[4] = { 9, 9, 9, 9 };
    CHECK(ring.popOrZero(out, 4) == true);   // underrun reported
    CHECK_NEAR(out[0], 5.0, 1e-9);
    CHECK_NEAR(out[1], 6.0, 1e-9);
    CHECK_NEAR(out[2], 0.0, 1e-9);           // shortfall zero-filled
    CHECK_NEAR(out[3], 0.0, 1e-9);
}

// The property that actually matters: no lost or duplicated samples when
// producer and consumer run concurrently on different cores.
void testConcurrentIntegrity() {
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

int main() {
    RUN(testBasicPushPop);
    RUN(testWrapAround);
    RUN(testOverflowIsClamped);
    RUN(testPopOrZeroSignalsUnderrun);
    RUN(testConcurrentIntegrity);
    TEST_MAIN_END
}
