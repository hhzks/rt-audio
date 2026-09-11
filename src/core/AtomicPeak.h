#pragma once
#include <algorithm>
#include <atomic>

namespace rt {

template <class A>
void raisePeak(A& a, float v) noexcept {
    float prev = a.load(std::memory_order_relaxed);
    while (!a.compare_exchange_weak(prev, std::max(prev, v), std::memory_order_relaxed)) {}
}

} // namespace rt
