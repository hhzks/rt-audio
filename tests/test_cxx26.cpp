#include <version>

#if !defined(__cpp_contracts) || __cpp_contracts < 202502L
#error "test_cxx26 needs P2900 contracts: GCC 16.2 or later"
#endif
#if !defined(__cpp_lib_inplace_vector) || __cpp_lib_inplace_vector < 202603L
#error "test_cxx26 needs std::inplace_vector with the 202603 API: GCC 16.2 or later"
#endif
#if !defined(__cpp_lib_atomic_min_max) || __cpp_lib_atomic_min_max < 202403L
#error "test_cxx26 needs std::atomic::fetch_max: GCC 16.2 or later"
#endif

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <contracts>
#include <cstdint>
#include <inplace_vector>

namespace {

std::atomic<int> g_violations{0};
std::atomic<int> g_lastKind{-1};

int checkedIndex(int i, int n) pre(i >= 0 && i < n) { return i; }

void checkedAssert(bool ok) { contract_assert(ok); }

} // namespace

void handle_contract_violation(const std::contracts::contract_violation& v) {
    g_violations.fetch_add(1, std::memory_order_relaxed);
    g_lastKind.store(static_cast<int>(v.kind()), std::memory_order_relaxed);
}

TEST_CASE("inplace_vector fills to capacity and refuses more without an exception", "[toolchain]") {
    std::inplace_vector<int, 4> v;
    for (int i = 0; i < 4; ++i)
        v.push_back(i);
    REQUIRE(v.size() == 4);

    bool accepted = true;
    REQUIRE_NOTHROW(accepted = static_cast<bool>(v.try_push_back(99)));
    CHECK_FALSE(accepted);
    CHECK(v.size() == 4);
    CHECK(v.back() == 3);
}

TEST_CASE("a met precondition does not call the handler", "[toolchain]") {
    const int before = g_violations.load();
    CHECK(checkedIndex(1, 2) == 1);
    CHECK(g_violations.load() == before);
}

TEST_CASE("a failed precondition calls the replaced handler and the test continues", "[toolchain]") {
    const int before = g_violations.load();
    (void)checkedIndex(5, 2);
    CHECK(g_violations.load() == before + 1);
    CHECK(g_lastKind.load() == static_cast<int>(std::contracts::assertion_kind::pre));
}

TEST_CASE("a failed contract_assert calls the handler with kind assert", "[toolchain]") {
    const int before = g_violations.load();
    checkedAssert(false);
    CHECK(g_violations.load() == before + 1);
    CHECK(g_lastKind.load() == static_cast<int>(std::contracts::assertion_kind::assert));
}

TEST_CASE("fetch_max keeps the larger value", "[toolchain]") {
    std::atomic<std::int64_t> peak{10};
    peak.fetch_max(25);
    CHECK(peak.load() == 25);
    peak.fetch_max(7);
    CHECK(peak.load() == 25);
}
