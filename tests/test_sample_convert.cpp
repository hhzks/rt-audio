#include "core/SampleConvert.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <vector>

using namespace rt;
using Catch::Matchers::WithinAbs;

namespace {

double lsb(SampleFormat f) {
    switch (f) {
        case SampleFormat::Int16: return 1.0 / 32768.0;
        case SampleFormat::Int24: return 1.0 / 8388608.0;
        case SampleFormat::Int32: return 1.0 / 2147483648.0;
        case SampleFormat::Float32: return 0.0;
    }
    return 0.0;
}

std::int32_t encodeOne(float v, SampleFormat f) {
    unsigned char b[4] = {};
    fromFloat(&v, b, 1, f);
    switch (f) {
        case SampleFormat::Int16: {
            std::int16_t s; std::memcpy(&s, b, sizeof(s)); return s;
        }
        case SampleFormat::Int24: return detail::readInt24(b);
        case SampleFormat::Int32: {
            std::int32_t s; std::memcpy(&s, b, sizeof(s)); return s;
        }
        default: return 0;
    }
}

} // namespace

TEST_CASE("byte widths", "[core]") {
    CHECK(bytesPerSample(SampleFormat::Int16) == 2);
    CHECK(bytesPerSample(SampleFormat::Int24) == 3);
    CHECK(bytesPerSample(SampleFormat::Int32) == 4);
    CHECK(bytesPerSample(SampleFormat::Float32) == 4);
}

// The property that matters: a waveshaper with drive up produces samples well
// outside [-1,1], and those must saturate rather than wrap to the opposite sign.
TEST_CASE("overdrive clamps instead of wrapping", "[core]") {
    struct { SampleFormat f; std::int32_t lo, hi; } cases[] = {
        { SampleFormat::Int16, -32768, 32767 },
        { SampleFormat::Int24, -8388608, 8388607 },
        { SampleFormat::Int32, INT32_MIN, INT32_MAX },
    };
    for (const auto& c : cases) {
        CAPTURE(c.f);
        for (float over : { 1.0f, 1.5f, 12.0f, 1e6f }) {
            CAPTURE(over);
            CHECK(encodeOne( over, c.f) == c.hi);
            CHECK(encodeOne(-over, c.f) == c.lo);
        }
        CHECK(encodeOne(0.0f, c.f) == 0);
    }
}

TEST_CASE("round trip is within half an LSB", "[core]") {
    const float in[] = { 0.0f, 0.25f, -0.25f, 0.5f, -0.5f, 0.999f, -0.999f, 0.001f };
    constexpr std::size_t n = sizeof(in) / sizeof(in[0]);

    for (SampleFormat f : { SampleFormat::Int16, SampleFormat::Int24, SampleFormat::Int32 }) {
        CAPTURE(f);
        std::vector<unsigned char> buf(n * static_cast<std::size_t>(bytesPerSample(f)));
        float out[n] = {};
        fromFloat(in, buf.data(), n, f);
        toFloat(buf.data(), out, n, f);
        for (std::size_t i = 0; i < n; ++i)
            CHECK_THAT(out[i], WithinAbs(static_cast<double>(in[i]), lsb(f) * 0.5 + 1e-12));
    }
}

TEST_CASE("float32 output is clamped to full scale", "[core]") {
    const float in[] = { -2.0f, -0.5f, 0.0f, 0.5f, 3.0f };
    const float want[] = { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f };
    float mid[5] = {}, out[5] = {};
    fromFloat(in, mid, 5, SampleFormat::Float32);
    toFloat(mid, out, 5, SampleFormat::Float32);
    for (int i = 0; i < 5; ++i) CHECK(out[i] == want[i]);
}

TEST_CASE("a stride reads and writes every n-th float", "[core]") {
    for (SampleFormat f : { SampleFormat::Int16, SampleFormat::Int24, SampleFormat::Int32,
                            SampleFormat::Float32 }) {
        CAPTURE(f);
        const float inter[] = { 0.5f, 9.0f, -0.25f, 9.0f, 0.125f, 9.0f };
        std::vector<unsigned char> buf(3 * static_cast<std::size_t>(bytesPerSample(f)));
        fromFloat(inter, buf.data(), 3, f, 2);
        float back[6] = { 7.0f, 7.0f, 7.0f, 7.0f, 7.0f, 7.0f };
        toFloat(buf.data(), back + 1, 3, f, 2);
        CHECK(back[0] == 7.0f);
        CHECK(back[1] == 0.5f);
        CHECK(back[2] == 7.0f);
        CHECK(back[3] == -0.25f);
        CHECK(back[4] == 7.0f);
        CHECK(back[5] == 0.125f);
    }
}

// 24-bit is three packed bytes with no native type; sign extension of bit 23 is
// hand-rolled, so negative values are worth testing directly.
TEST_CASE("int24 sign extension", "[core]") {
    const unsigned char minusOne[3] = { 0xFF, 0xFF, 0xFF };
    CHECK(detail::readInt24(minusOne) == -1);

    const unsigned char mostNegative[3] = { 0x00, 0x00, 0x80 };
    CHECK(detail::readInt24(mostNegative) == -8388608);

    const unsigned char mostPositive[3] = { 0xFF, 0xFF, 0x7F };
    CHECK(detail::readInt24(mostPositive) == 8388607);

    for (std::int32_t v : { -8388608, -4242, -1, 0, 1, 4242, 8388607 }) {
        CAPTURE(v);
        unsigned char b[3] = {};
        detail::writeInt24(b, v);
        CHECK(detail::readInt24(b) == v);
    }
}
