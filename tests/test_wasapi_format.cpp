#include "io/wasapi/WasapiFormat.h"
#include <catch2/catch_test_macros.hpp>

#include <string>

namespace Catch {
template <> struct StringMaker<rt::FormatCandidate> {
    static std::string convert(const rt::FormatCandidate& f) {
        return std::to_string(f.channels) + " ch, " + std::to_string(f.validBits) + " of "
             + std::to_string(f.bits) + " bits" + (f.isFloat ? " float" : "");
    }
};
} // namespace Catch

using namespace rt;

TEST_CASE("float keeps priority over integer formats", "[io]") {
    const auto f = pickExclusiveFormat(2, [](const FormatCandidate& c) {
        return c.isFloat || (c.bits == 16 && !c.isFloat);
    });
    REQUIRE(f);
    CHECK(*f == FormatCandidate{ 2, 32, 32, true });
}

TEST_CASE("a device that accepts 16-bit and 24-bit in 32 gets 24-bit in 32", "[io]") {
    const auto f = pickExclusiveFormat(2, [](const FormatCandidate& c) {
        return !c.isFloat && ((c.bits == 32 && c.validBits == 24) || c.bits == 16);
    });
    REQUIRE(f);
    CHECK(*f == FormatCandidate{ 2, 32, 24, false });
}

TEST_CASE("a device that rejects stereo falls back to mono", "[io]") {
    const auto f = pickExclusiveFormat(2, [](const FormatCandidate& c) {
        return c.channels == 1 && c.bits == 16;
    });
    REQUIRE(f);
    CHECK(*f == FormatCandidate{ 1, 16, 16, false });
}

TEST_CASE("the format text names the valid bits of a wider container", "[io]") {
    CHECK(formatText("32-bit PCM", 32, 24) == "24-bit in 32-bit PCM");
    CHECK(formatText("32-bit PCM", 32, 32) == "32-bit PCM");
    CHECK(formatText("16-bit PCM", 16, 0) == "16-bit PCM");
}

TEST_CASE("a device that accepts nothing gets no format", "[io]") {
    CHECK(!pickExclusiveFormat(2, [](const FormatCandidate&) { return false; }));
}
