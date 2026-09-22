#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace rt {

enum class Taper : std::uint8_t { Linear = 0, Log = 1 };

enum ParamFlag : std::uint8_t { kReadOnly = 1, kToggle = 2 };

struct ParamInfo {
    const char*  id;
    const char*  name;
    const char*  unit;
    double       min, max, def;
    Taper        taper;
    std::uint8_t flags;
};

static_assert(std::atomic<double>::is_always_lock_free,
              "atomic<double> must be lock-free or the audio thread can block");

// set(): control thread only. publish(): audio thread, read-only params only.
template <std::size_t N>
class ParamBlock {
public:
    explicit ParamBlock(const std::array<ParamInfo, N>& info) noexcept : info_(info) {
        for (std::size_t i = 0; i < N; ++i) {
            defaults_[i] = info[i].def;
            values_[i].store(info[i].def, std::memory_order_relaxed);
        }
    }

    void setDefault(std::size_t i, double v) noexcept pre(i < N) {
        defaults_[i] = v;
        values_[i].store(v, std::memory_order_relaxed);
    }

    double get(std::size_t i) const noexcept pre(i < N) { return values_[i].load(std::memory_order_relaxed); }
    double defaultValue(std::size_t i) const noexcept pre(i < N) { return defaults_[i]; }

    bool set(std::size_t i, double v) noexcept {
        if (i >= N) return false;
        const ParamInfo& p = info_[i];
        if ((p.flags & kReadOnly) != 0) return false;
        if (!std::isfinite(v)) return false;
        if ((p.flags & kToggle) != 0) v = v >= 0.5 ? 1.0 : 0.0;
        else                          v = std::clamp(v, p.min, p.max);
        values_[i].store(v, std::memory_order_relaxed);
        return true;
    }

    void publish(std::size_t i, double v) noexcept pre(i < N && (info_[i].flags & kReadOnly) != 0) {
        values_[i].store(v, std::memory_order_relaxed);
    }

    std::span<const ParamInfo> info() const noexcept { return info_; }

private:
    const std::array<ParamInfo, N>&    info_;
    std::array<double, N>              defaults_{};
    std::array<std::atomic<double>, N> values_{};
};

} // namespace rt
