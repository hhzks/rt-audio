#include "io/Utf8Console.h"
#include <catch2/catch_test_macros.hpp>

using namespace rt;

TEST_CASE("a console signal restores the previous code page", "[io]") {
    const UINT original = GetConsoleOutputCP();
    if (original == 0) {
        SUCCEED("no console");
        return;
    }
    SetConsoleOutputCP(437);
    {
        Utf8Console console;
        CHECK(GetConsoleOutputCP() == CP_UTF8);
        CHECK(Utf8Console::restoreOnSignal(CTRL_C_EVENT) == FALSE);
        CHECK(GetConsoleOutputCP() == 437);
    }
    CHECK(GetConsoleOutputCP() == 437);
    SetConsoleOutputCP(original);
}
