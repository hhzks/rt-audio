#pragma once
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>

namespace rt {

// Lock-free single-producer / single-consumer float ring.
// Capacity is rounded up to a power of two so the index wrap is a mask.
class SpscRingBuffer {
public:
    void reset(std::size_t minCapacity) {
        std::size_t cap = 1;
        while (cap < minCapacity) cap <<= 1;
        capacity_ = cap;
        mask_     = cap - 1;
        data_     = std::make_unique<float[]>(cap);
        std::memset(data_.get(), 0, cap * sizeof(float));
        writeIdx_.store(0, std::memory_order_relaxed);
        readIdx_.store(0, std::memory_order_relaxed);
    }

    std::size_t capacity() const noexcept { return capacity_; }

    // --- producer side ---
    std::size_t writeAvailable() const noexcept {
        const auto w = writeIdx_.load(std::memory_order_relaxed);
        const auto r = readIdx_.load(std::memory_order_acquire);
        return capacity_ - (w - r) - 1; // keep one slot free to disambiguate full/empty
    }

    std::size_t push(const float* src, std::size_t count) noexcept {
        const std::size_t avail = writeAvailable();
        const std::size_t n = count < avail ? count : avail;
        auto w = writeIdx_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < n; ++i)
            data_[(w + i) & mask_] = src[i];
        writeIdx_.store(w + n, std::memory_order_release);
        return n;
    }

    // --- consumer side ---
    std::size_t readAvailable() const noexcept {
        const auto w = writeIdx_.load(std::memory_order_acquire);
        const auto r = readIdx_.load(std::memory_order_relaxed);
        return w - r;
    }

    std::size_t pop(float* dst, std::size_t count) noexcept {
        const std::size_t avail = readAvailable();
        const std::size_t n = count < avail ? count : avail;
        auto r = readIdx_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < n; ++i)
            dst[i] = data_[(r + i) & mask_];
        readIdx_.store(r + n, std::memory_order_release);
        return n;
    }

    // Pop exactly `count`, zero-filling any shortfall. Returns true if the
    // buffer ran dry (an underrun -- count it, do not log from the RT thread).
    bool popOrZero(float* dst, std::size_t count) noexcept {
        const std::size_t got = pop(dst, count);
        if (got < count) {
            std::memset(dst + got, 0, (count - got) * sizeof(float));
            return true;
        }
        return false;
    }

    // Drop the oldest `count` samples. Used by the drift compensator when the
    // buffer creeps toward full.
    void discard(std::size_t count) noexcept {
        const std::size_t avail = readAvailable();
        const std::size_t n = count < avail ? count : avail;
        const auto r = readIdx_.load(std::memory_order_relaxed);
        readIdx_.store(r + n, std::memory_order_release);
    }

private:
    static constexpr std::size_t kCacheLine = 64;

    std::unique_ptr<float[]> data_;
    std::size_t capacity_ = 0;
    std::size_t mask_     = 0;

    alignas(kCacheLine) std::atomic<std::size_t> writeIdx_{0};
    alignas(kCacheLine) std::atomic<std::size_t> readIdx_{0};
    [[maybe_unused]] char pad_[kCacheLine]{};   // keep the two indices off each other's cache line
};

} // namespace rt
