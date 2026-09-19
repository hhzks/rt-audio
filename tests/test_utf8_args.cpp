#include "io/Utf8Args.h"
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace rt;

#ifdef _WIN32
TEST_CASE("a wide command line becomes UTF-8 arguments", "[io]") {
    const std::vector<std::string> args = utf8ArgsFrom(L"prog.exe \"ASIO® Test\" --in x");
    REQUIRE(args.size() == 4);
    CHECK(args[0] == "prog.exe");
    CHECK(args[1] == "ASIO\xC2\xAE Test");
    CHECK(args[2] == "--in");
    CHECK(args[3] == "x");
}
#else
TEST_CASE("argv is copied as it is", "[io]") {
    char prog[] = "prog", arg[] = "ASIO\xC2\xAE";
    char* argv[] = {prog, arg};
    CHECK(utf8Args(2, argv) == std::vector<std::string>{"prog", "ASIO\xC2\xAE"});
}
#endif
