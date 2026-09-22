#include <catch2/catch_test_macros.hpp>

namespace {

int checkedIndex(int i, int n) pre(i >= 0 && i < n) { return i; }

} // namespace

TEST_CASE("the gate fails a test that causes a contract violation", "[gate]") {
    CHECK(checkedIndex(5, 2) == 5);
}
