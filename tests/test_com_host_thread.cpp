#include "io/asio/ComHostThread.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <objbase.h>

#include <atomic>
#include <future>
#include <stdexcept>

using namespace rt;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("run executes on one other thread", "[io]") {
    ComHostThread host;
    DWORD first = 0, second = 0;
    host.run([&] { first = GetCurrentThreadId(); });
    host.run([&] { second = GetCurrentThreadId(); });
    CHECK(first != 0);
    CHECK(first == second);
    CHECK(first != GetCurrentThreadId());
}

TEST_CASE("run passes an exception back to the caller", "[io]") {
    ComHostThread host;
    CHECK_THROWS_WITH(host.run([] { throw std::runtime_error("from the host"); }),
                      ContainsSubstring("from the host"));
    int after = 0;
    host.run([&] { after = 1; });
    CHECK(after == 1);
}

TEST_CASE("the host thread is a single-threaded apartment with a window", "[io]") {
    ComHostThread host;
    APTTYPE type{};
    APTTYPEQUALIFIER qualifier{};
    HRESULT hr = E_FAIL;
    bool window = false;
    host.run([&] {
        hr = CoGetApartmentType(&type, &qualifier);
        window = IsWindow(host.window()) != FALSE;
    });
    REQUIRE(SUCCEEDED(hr));
    CHECK((type == APTTYPE_STA || type == APTTYPE_MAINSTA));
    CHECK(window);
}

TEST_CASE("post returns before the task ends", "[io]") {
    ComHostThread host;
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    std::atomic<bool> finished{false};
    host.post([gate, &finished] {
        gate.wait();
        finished = true;
    });
    CHECK(!finished.load());
    release.set_value();
    host.run([] {});
    CHECK(finished.load());
}

TEST_CASE("run from the host thread itself does not deadlock", "[io]") {
    ComHostThread host;
    int inner = 0;
    host.run([&] { host.run([&] { inner = 1; }); });
    CHECK(inner == 1);
}
