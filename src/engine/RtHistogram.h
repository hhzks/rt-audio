#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

#include "core/Types.h"

namespace rt {

class RtHistogram {
public:
    static constexpr int kSubBits     = 4;
    static constexpr int kSubCount    = 1 << kSubBits;
    static constexpr int kMinExp      = 8;
    static constexpr int kMaxExp      = 27;
    static constexpr int kBucketCount = (kMaxExp - kMinExp + 1) * kSubCount + 2;

    static constexpr int bucketFor(std::uint64_t ns) noexcept {
        if (ns < (1ull << kMinExp)) return 0;
        const int exp = 63 - std::countl_zero(ns);
        if (exp > kMaxExp) return kBucketCount - 1;
        const int shift = exp - kSubBits;
        const auto m = static_cast<int>((ns >> shift) & (kSubCount - 1));
        return 1 + (exp - kMinExp) * kSubCount + m;
    }

    static constexpr std::uint64_t bucketLowerNs(int b) noexcept {
        if (b <= 0) return 0;
        if (b >= kBucketCount - 1) return 1ull << (kMaxExp + 1);
        const int exp = kMinExp + (b - 1) / kSubCount;
        const int m   = (b - 1) % kSubCount;
        return static_cast<std::uint64_t>(kSubCount + m) << (exp - kSubBits);
    }

    static constexpr std::uint64_t bucketUpperNs(int b) noexcept {
        if (b <= 0) return 1ull << kMinExp;
        if (b >= kBucketCount - 1) return UINT64_MAX;
        const int exp = kMinExp + (b - 1) / kSubCount;
        return bucketLowerNs(b) + (1ull << (exp - kSubBits));
    }
};

struct HistogramSnapshot {
    std::array<std::uint64_t, idx(RtHistogram::kBucketCount)> counts{};
    std::uint64_t total = 0;

    void add(const HistogramSnapshot& other) noexcept {
        for (std::size_t i = 0; i < counts.size(); ++i) counts[i] += other.counts[i];
        total += other.total;
    }

    std::uint64_t nsAtPercentile(double p) const noexcept {
        if (total == 0) return 0;
        p = std::clamp(p, 0.0, 1.0);
        const double threshold = p * static_cast<double>(total);
        std::uint64_t cum = 0;
        for (int b = 0; b < RtHistogram::kBucketCount; ++b) {
            cum += counts[idx(b)];
            if (static_cast<double>(cum) >= threshold && counts[idx(b)] != 0)
                return RtHistogram::bucketUpperNs(b);
        }
        return maxNs();
    }

    std::uint64_t maxNs() const noexcept {
        for (int b = RtHistogram::kBucketCount - 1; b >= 0; --b)
            if (counts[idx(b)] != 0) return RtHistogram::bucketUpperNs(b);
        return 0;
    }

    std::uint64_t underflow() const noexcept { return counts[0]; }
    std::uint64_t overflow()  const noexcept { return counts[idx(RtHistogram::kBucketCount - 1)]; }
};

} // namespace rt
