#include "engine/RtHistogram.h"
#include "TestHarness.h"
#include "core/Types.h"

#include <array>
#include <cstdint>

using namespace rt;

void testBucketMonotonic() {
    int prev = RtHistogram::bucketFor(0);
    for (std::uint64_t ns = 1; ns < 200'000'000ull; ns += ns / 64 + 1) {
        const int b = RtHistogram::bucketFor(ns);
        CHECK(b >= prev);
        prev = b;
    }
}

void testBucketRoundTrip() {
    for (std::uint64_t ns = 256; ns < (1ull << 28); ns += ns / 32 + 1) {
        const int b = RtHistogram::bucketFor(ns);
        CHECK(RtHistogram::bucketLowerNs(b) <= ns);
        CHECK(ns < RtHistogram::bucketUpperNs(b));
    }
}

void testBucketRelativeWidth() {
    for (int b = 1; b < RtHistogram::kBucketCount - 1; ++b) {
        const auto lo = RtHistogram::bucketLowerNs(b);
        const auto hi = RtHistogram::bucketUpperNs(b);
        CHECK(hi > lo);
        const double rel = static_cast<double>(hi - lo) / static_cast<double>(lo);
        CHECK(rel <= 1.0 / 16.0 + 1e-12);
    }
}

void testUnderflowOverflow() {
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

void testPercentiles() {
    HistogramSnapshot s;
    put(s, 20'000, 990);        // 20 us
    put(s, 2'000'000, 10);      // 2 ms
    CHECK(s.total == 1000);

    CHECK(s.nsAtPercentile(0.5) >= 20'000);
    CHECK(s.nsAtPercentile(0.5) <  40'000);
    CHECK(s.nsAtPercentile(0.999) >= 2'000'000);
    CHECK(s.maxNs() >= 2'000'000);
}

void testEmptySnapshot() {
    HistogramSnapshot s;
    CHECK(s.nsAtPercentile(0.5) == 0);
    CHECK(s.maxNs() == 0);
    CHECK(s.total == 0);
}

void testPercentileClamped() {
    HistogramSnapshot s;
    put(s, 20'000, 990);
    put(s, 2'000'000, 10);
    CHECK(s.nsAtPercentile(0.0) <  100'000);
    CHECK(s.nsAtPercentile(1.0) >= 2'000'000);
    CHECK(s.nsAtPercentile(-1.0) == s.nsAtPercentile(0.0));
    CHECK(s.nsAtPercentile(2.0)  == s.nsAtPercentile(1.0));
}

void testPercentileNoTruncationBias() {
    HistogramSnapshot s;
    put(s, 20'000, 28);
    put(s, 2'000'000, 72);
    CHECK(s.nsAtPercentile(0.29) >= 2'000'000);
}

void testSnapshotAdd() {
    HistogramSnapshot a, b;
    put(a, 20'000, 5);
    put(b, 20'000, 7);
    put(b, 2'000'000, 1);
    a.add(b);
    CHECK(a.total == 13);
    CHECK(a.counts[idx(RtHistogram::bucketFor(20'000))] == 12);
    CHECK(a.counts[idx(RtHistogram::bucketFor(2'000'000))] == 1);
}

void testRecordAndPeek() {
    RtHistogram h;
    for (int i = 0; i < 100; ++i) h.record(20'000);
    h.record(2'000'000);
    const auto s = h.peek();
    CHECK(s.total == 101);
    CHECK(s.counts[idx(RtHistogram::bucketFor(20'000))] == 100);
    CHECK(s.maxNs() >= 2'000'000);
}

void testPeekIsNonDestructive() {
    RtHistogram h;
    h.record(20'000);
    CHECK(h.peek().total == 1);
    CHECK(h.peek().total == 1);
}

void testDrainIsDestructiveAndLossless() {
    RtHistogram h;
    for (int i = 0; i < 50; ++i) h.record(20'000);
    const auto first = h.drain();
    CHECK(first.total == 50);
    const auto second = h.drain();
    CHECK(second.total == 0);
    CHECK(h.peek().total == 0);
}

void testReset() {
    RtHistogram h;
    h.record(20'000);
    h.reset();
    CHECK(h.peek().total == 0);
}

int main() {
    RUN(testBucketMonotonic);
    RUN(testBucketRoundTrip);
    RUN(testBucketRelativeWidth);
    RUN(testUnderflowOverflow);
    RUN(testPercentiles);
    RUN(testEmptySnapshot);
    RUN(testPercentileClamped);
    RUN(testPercentileNoTruncationBias);
    RUN(testSnapshotAdd);
    RUN(testRecordAndPeek);
    RUN(testPeekIsNonDestructive);
    RUN(testDrainIsDestructiveAndLossless);
    RUN(testReset);
    TEST_MAIN_END
}
