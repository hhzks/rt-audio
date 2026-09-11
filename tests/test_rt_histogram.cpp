#include "engine/RtHistogram.h"
#include <catch2/catch_test_macros.hpp>
#include "core/Types.h"
#include "engine/AudioEngine.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using namespace rt;

TEST_CASE("bucket index is monotonic", "[engine]") {
    int prev = RtHistogram::bucketFor(0);
    for (std::uint64_t ns = 1; ns < 200'000'000ull; ns += ns / 64 + 1) {
        CAPTURE(ns);
        const int b = RtHistogram::bucketFor(ns);
        CHECK(b >= prev);
        prev = b;
    }
}

TEST_CASE("bucket bounds contain the value", "[engine]") {
    for (std::uint64_t ns = 256; ns < (1ull << 28); ns += ns / 32 + 1) {
        CAPTURE(ns);
        const int b = RtHistogram::bucketFor(ns);
        CHECK(RtHistogram::bucketLowerNs(b) <= ns);
        CHECK(ns < RtHistogram::bucketUpperNs(b));
    }
}

TEST_CASE("bucket relative width", "[engine]") {
    for (int b = 1; b < RtHistogram::kBucketCount - 1; ++b) {
        CAPTURE(b);
        const auto lo = RtHistogram::bucketLowerNs(b);
        const auto hi = RtHistogram::bucketUpperNs(b);
        CHECK(hi > lo);
        const double rel = static_cast<double>(hi - lo) / static_cast<double>(lo);
        CHECK(rel <= 1.0 / 16.0 + 1e-12);
    }
}

TEST_CASE("underflow and overflow buckets", "[engine]") {
    CHECK(RtHistogram::bucketFor(0)   == 0);
    CHECK(RtHistogram::bucketFor(255) == 0);
    CHECK(RtHistogram::bucketFor(256) == 1);
    CHECK(RtHistogram::bucketFor(1ull << 27) == 305);
    CHECK(RtHistogram::bucketFor((1ull << 28) - 1) == 320);
    CHECK(RtHistogram::bucketFor(1ull << 28) == RtHistogram::kBucketCount - 1);
    CHECK(RtHistogram::kBucketCount == 322);
}

namespace {
void put(HistogramSnapshot& s, std::uint64_t ns, std::uint64_t n) {
    s.counts[idx(RtHistogram::bucketFor(ns))] += n;
    s.total += n;
}
}

TEST_CASE("percentiles", "[engine]") {
    HistogramSnapshot s;
    put(s, 20'000, 990);        // 20 us
    put(s, 2'000'000, 10);      // 2 ms
    CHECK(s.total == 1000);

    CHECK(s.nsAtPercentile(0.5) >= 20'000);
    CHECK(s.nsAtPercentile(0.5) <  40'000);
    CHECK(s.nsAtPercentile(0.999) >= 2'000'000);
    CHECK(s.maxNs() >= 2'000'000);
}

TEST_CASE("empty snapshot", "[engine]") {
    HistogramSnapshot s;
    CHECK(s.nsAtPercentile(0.5) == 0);
    CHECK(s.maxNs() == 0);
    CHECK(s.total == 0);
}

TEST_CASE("percentile is clamped", "[engine]") {
    HistogramSnapshot s;
    put(s, 20'000, 990);
    put(s, 2'000'000, 10);
    CHECK(s.nsAtPercentile(0.0) <  100'000);
    CHECK(s.nsAtPercentile(1.0) >= 2'000'000);
    CHECK(s.nsAtPercentile(-1.0) == s.nsAtPercentile(0.0));
    CHECK(s.nsAtPercentile(2.0)  == s.nsAtPercentile(1.0));
}

TEST_CASE("percentile has no truncation bias", "[engine]") {
    HistogramSnapshot s;
    put(s, 20'000, 28);
    put(s, 2'000'000, 72);
    CHECK(s.nsAtPercentile(0.29) >= 2'000'000);
}

TEST_CASE("snapshot add", "[engine]") {
    HistogramSnapshot a, b;
    put(a, 20'000, 5);
    put(b, 20'000, 7);
    put(b, 2'000'000, 1);
    a.add(b);
    CHECK(a.total == 13);
    CHECK(a.counts[idx(RtHistogram::bucketFor(20'000))] == 12);
    CHECK(a.counts[idx(RtHistogram::bucketFor(2'000'000))] == 1);
}

TEST_CASE("record and peek", "[engine]") {
    RtHistogram h;
    for (int i = 0; i < 100; ++i) h.record(20'000);
    h.record(2'000'000);
    const auto s = h.peek();
    CHECK(s.total == 101);
    CHECK(s.counts[idx(RtHistogram::bucketFor(20'000))] == 100);
    CHECK(s.maxNs() >= 2'000'000);
}

TEST_CASE("peek is non-destructive", "[engine]") {
    RtHistogram h;
    h.record(20'000);
    CHECK(h.peek().total == 1);
    CHECK(h.peek().total == 1);
}

TEST_CASE("drain is destructive and lossless", "[engine]") {
    RtHistogram h;
    for (int i = 0; i < 50; ++i) h.record(20'000);
    const auto first = h.drain();
    CHECK(first.total == 50);
    const auto second = h.drain();
    CHECK(second.total == 0);
    CHECK(h.peek().total == 0);
}

TEST_CASE("reset", "[engine]") {
    RtHistogram h;
    h.record(20'000);
    h.reset();
    CHECK(h.peek().total == 0);
}

TEST_CASE("concurrent drain loses nothing", "[engine]") {
    constexpr int kWriters          = 3;
    constexpr int kRecordsPerWriter = 50'000;
    constexpr int kTrials           = 20;

    for (int trial = 0; trial < kTrials; ++trial) {
        CAPTURE(trial);
        RtHistogram h;
        std::atomic<int> finished{0};

        std::vector<std::thread> writers;
        for (int w = 0; w < kWriters; ++w) {
            writers.emplace_back([&h, &finished] {
                for (int i = 0; i < kRecordsPerWriter; ++i)
                    h.record(20'000 + static_cast<std::uint64_t>(i % 97));
                finished.fetch_add(1, std::memory_order_release);
            });
        }

        HistogramSnapshot cumulative;
        while (finished.load(std::memory_order_acquire) < kWriters)
            cumulative.add(h.drain());

        for (auto& t : writers) t.join();
        cumulative.add(h.drain());

        CHECK(cumulative.total ==
              static_cast<std::uint64_t>(kWriters) * kRecordsPerWriter);
    }
}

TEST_CASE("overflow is reported at the ceiling", "[engine]") {
    RtHistogram h;
    h.record(20'000);
    h.record(1ull << 29);
    const auto s = h.peek();
    CHECK(s.overflow() == 1);
    CHECK(s.maxNs() == (1ull << (RtHistogram::kMaxExp + 1)));
    CHECK(s.maxNs() != UINT64_MAX);
}

TEST_CASE("engine records every callback after warm-up", "[engine]") {
    AudioEngine engine;
    engine.prepare(48000.0, 128, 2);
    CHECK(engine.stats().callbackNanos.peek().total == 0);

    std::vector<float> in(128 * 2, 0.1f), out(128 * 2);
    for (int i = 0; i < 50; ++i)
        engine.processInterleaved(in.data(), out.data(), 128);

    CHECK(engine.stats().callbackNanos.peek().total == 50);
    CHECK(engine.stats().loadFactorAt(0.5) > 0.0);
    CHECK(engine.stats().loadFactorAt(0.5) < 1.0);
}
