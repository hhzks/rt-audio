#pragma once
#include <bit>
#include <cstdint>

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

} // namespace rt
