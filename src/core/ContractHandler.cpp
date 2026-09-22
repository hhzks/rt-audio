#include "core/ContractHandler.h"

#include <atomic>
#include <contracts>
#include <cstdio>

namespace {

std::atomic<std::uint64_t> g_count{0};
std::atomic<int>           g_state{0};   // 0 empty, 1 writing, 2 ready
rt::contracts::Violation   g_first;

} // namespace

// Runs on the audio thread under observe: atomics and plain stores only.
void handle_contract_violation(const std::contracts::contract_violation& v) {
    g_count.fetch_add(1, std::memory_order_relaxed);
    const auto loc = v.location();
    int expected = 0;
    if (g_state.compare_exchange_strong(expected, 1, std::memory_order_relaxed)) {
        g_first = {loc.file_name(), loc.function_name(), v.comment(),
                   static_cast<unsigned>(loc.line())};
        g_state.store(2, std::memory_order_release);
    }
    if (v.is_terminating())
        std::fprintf(stderr, "rt-audio: contract violation at %s:%u in %s: %s\n", loc.file_name(),
                     static_cast<unsigned>(loc.line()), loc.function_name(), v.comment());
}

namespace rt::contracts {

std::uint64_t violationCount() noexcept { return g_count.load(std::memory_order_relaxed); }

bool firstViolation(Violation& out) noexcept {
    if (g_state.load(std::memory_order_acquire) != 2) return false;
    out = g_first;
    return true;
}

ExitReport::~ExitReport() {
    const auto n = static_cast<unsigned long long>(violationCount());
    if (n == 0) return;
    Violation first;
    if (firstViolation(first))
        std::fprintf(stderr, "rt-audio: %llu contract violation(s); first at %s:%u in %s: %s\n", n,
                     first.file, first.line, first.function, first.predicate);
    else
        std::fprintf(stderr, "rt-audio: %llu contract violation(s)\n", n);
}

} // namespace rt::contracts
