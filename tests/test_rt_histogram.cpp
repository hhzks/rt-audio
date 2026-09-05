#include "engine/RtHistogram.h"
#include "TestHarness.h"

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
    for (std::uint64_t ns = 256; ns < 134'000'000ull; ns += ns / 32 + 1) {
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
    CHECK(RtHistogram::bucketFor(1ull << 28) == RtHistogram::kBucketCount - 1);
    CHECK(RtHistogram::kBucketCount == 322);
}

int main() {
    RUN(testBucketMonotonic);
    RUN(testBucketRoundTrip);
    RUN(testBucketRelativeWidth);
    RUN(testUnderflowOverflow);
    TEST_MAIN_END
}
