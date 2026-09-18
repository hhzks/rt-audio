#include "io/asio/AsioLogic.h"
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace rt;

TEST_CASE("block 0 and granularity 0 give the preferred size", "[io]") {
    CHECK(chooseAsioBufferSize(0, 64, 2048, 512, 8) == 512);
    CHECK(chooseAsioBufferSize(-1, 64, 2048, 512, 8) == 512);
    CHECK(chooseAsioBufferSize(128, 256, 256, 256, 0) == 256);
}

TEST_CASE("granularity -1 gives the nearest power of two, and a tie goes up", "[io]") {
    CHECK(chooseAsioBufferSize(100, 64, 2048, 256, -1) == 128);
    CHECK(chooseAsioBufferSize(96, 64, 2048, 256, -1) == 128);
    CHECK(chooseAsioBufferSize(64, 64, 2048, 256, -1) == 64);
    CHECK(chooseAsioBufferSize(3000, 64, 2048, 256, -1) == 2048);
    CHECK(chooseAsioBufferSize(10, 64, 2048, 256, -1) == 64);
}

TEST_CASE("a positive granularity steps from the minimum, and a tie goes up", "[io]") {
    CHECK(chooseAsioBufferSize(100, 64, 2048, 512, 8) == 104);
    CHECK(chooseAsioBufferSize(97, 64, 2048, 512, 8) == 96);
    CHECK(chooseAsioBufferSize(10, 64, 2048, 512, 8) == 64);
    CHECK(chooseAsioBufferSize(5000, 64, 2048, 512, 8) == 2048);
    CHECK(chooseAsioBufferSize(2045, 64, 2048, 512, 8) == 2048);
    CHECK(chooseAsioBufferSize(99, 64, 100, 64, 24) == 88);   // the next step would pass max
}

TEST_CASE("driver messages map to replies and actions", "[io]") {
    using S = AsioSelector;
    const auto msg = [](S s) { return handleAsioMessage(static_cast<long>(s), 0); };
    CHECK(msg(S::EngineVersion) == AsioMessageReply{2, AsioAction::None});
    CHECK(msg(S::ResetRequest) == AsioMessageReply{1, AsioAction::Reset});
    CHECK(msg(S::ResyncRequest) == AsioMessageReply{1, AsioAction::Xrun});
    CHECK(msg(S::Overload) == AsioMessageReply{1, AsioAction::Xrun});
    CHECK(msg(S::LatenciesChanged) == AsioMessageReply{1, AsioAction::LatenciesChanged});
    CHECK(msg(S::SupportsTimeInfo) == AsioMessageReply{1, AsioAction::None});
    CHECK(msg(S::BufferSizeChange) == AsioMessageReply{0, AsioAction::None});
    CHECK(handleAsioMessage(99, 0) == AsioMessageReply{0, AsioAction::None});
}

TEST_CASE("selector queries name only the handled selectors", "[io]") {
    const auto supported = [](long value) {
        return handleAsioMessage(static_cast<long>(AsioSelector::SelectorSupported), value).reply;
    };
    CHECK(supported(static_cast<long>(AsioSelector::ResetRequest)) == 1);
    CHECK(supported(static_cast<long>(AsioSelector::SupportsTimeInfo)) == 1);
    CHECK(supported(static_cast<long>(AsioSelector::Overload)) == 1);
    CHECK(supported(static_cast<long>(AsioSelector::BufferSizeChange)) == 0);
    CHECK(supported(99) == 0);
}

TEST_CASE("only little-endian integer and float types are supported", "[io]") {
    CHECK(asioTypeSupported(AsioSampleType::Int16LSB));
    CHECK(asioTypeSupported(AsioSampleType::Int32LSB));
    CHECK(asioTypeSupported(AsioSampleType::Int32LSB24));
    CHECK(asioTypeSupported(AsioSampleType::Float64LSB));
    CHECK(!asioTypeSupported(AsioSampleType::Int32MSB));
    CHECK(!asioTypeSupported(AsioSampleType::DSDInt8LSB1));
    CHECK(std::string(asioTypeName(AsioSampleType::Int32MSB)) == "Int32MSB");
    CHECK(std::string(asioTypeName(AsioSampleType::DSDInt8LSB1)) == "DSDInt8LSB1");
    CHECK(std::string(asioTypeName(static_cast<AsioSampleType>(99))) == "unknown");
}

namespace {

std::vector<unsigned char> bytes(std::initializer_list<int> v) {
    std::vector<unsigned char> b;
    for (int x : v) b.push_back(static_cast<unsigned char>(x));
    return b;
}

float decode(AsioSampleType t, const std::vector<unsigned char>& b) {
    float f = -9.0f;
    asioToFloat(b.data(), t, &f, 1, 1);
    return f;
}

std::vector<unsigned char> encode(AsioSampleType t, float v, std::size_t width) {
    std::vector<unsigned char> b(width, 0xAA);
    floatToAsio(&v, 1, b.data(), t, 1);
    return b;
}

} // namespace

TEST_CASE("integer types scale by their valid bits", "[io]") {
    CHECK(decode(AsioSampleType::Int16LSB, bytes({0x00, 0x40})) == 0.5f);
    CHECK(decode(AsioSampleType::Int16LSB, bytes({0x00, 0xC0})) == -0.5f);
    CHECK(decode(AsioSampleType::Int24LSB, bytes({0x00, 0x00, 0x40})) == 0.5f);
    CHECK(decode(AsioSampleType::Int24LSB, bytes({0x00, 0x00, 0xC0})) == -0.5f);
    CHECK(decode(AsioSampleType::Int32LSB, bytes({0x00, 0x00, 0x00, 0x40})) == 0.5f);
    CHECK(decode(AsioSampleType::Int32LSB, bytes({0x00, 0x00, 0x00, 0xC0})) == -0.5f);
    CHECK(decode(AsioSampleType::Int32LSB16, bytes({0x00, 0x40, 0x00, 0x00})) == 0.5f);
    CHECK(decode(AsioSampleType::Int32LSB18, bytes({0x00, 0x00, 0x01, 0x00})) == 0.5f);
    CHECK(decode(AsioSampleType::Int32LSB20, bytes({0x00, 0x00, 0x04, 0x00})) == 0.5f);
    CHECK(decode(AsioSampleType::Int32LSB24, bytes({0x00, 0x00, 0x40, 0x00})) == 0.5f);
}

TEST_CASE("right-justified types sign-extend from their top valid bit", "[io]") {
    CHECK(decode(AsioSampleType::Int32LSB24, bytes({0x00, 0x00, 0xC0, 0x00})) == -0.5f);
    CHECK(decode(AsioSampleType::Int32LSB24, bytes({0x00, 0x00, 0xC0, 0xFF})) == -0.5f);
    CHECK(decode(AsioSampleType::Int32LSB16, bytes({0x00, 0xC0, 0x00, 0x00})) == -0.5f);
}

TEST_CASE("float types pass through", "[io]") {
    const float q = 0.25f;
    std::vector<unsigned char> f(4);
    std::memcpy(f.data(), &q, 4);
    CHECK(decode(AsioSampleType::Float32LSB, f) == 0.25f);
    const double d = -0.75;
    std::vector<unsigned char> g(8);
    std::memcpy(g.data(), &d, 8);
    CHECK(decode(AsioSampleType::Float64LSB, g) == -0.75f);
}

TEST_CASE("encoding writes hand-checked bytes and clamps to full scale", "[io]") {
    CHECK(encode(AsioSampleType::Int16LSB, 0.5f, 2) == bytes({0x00, 0x40}));
    CHECK(encode(AsioSampleType::Int16LSB, 2.0f, 2) == bytes({0xFF, 0x7F}));
    CHECK(encode(AsioSampleType::Int16LSB, -2.0f, 2) == bytes({0x00, 0x80}));
    CHECK(encode(AsioSampleType::Int24LSB, -0.5f, 3) == bytes({0x00, 0x00, 0xC0}));
    CHECK(encode(AsioSampleType::Int32LSB, 0.5f, 4) == bytes({0x00, 0x00, 0x00, 0x40}));
    CHECK(encode(AsioSampleType::Int32LSB24, 0.5f, 4) == bytes({0x00, 0x00, 0x40, 0x00}));
    CHECK(encode(AsioSampleType::Int32LSB24, -0.5f, 4) == bytes({0x00, 0x00, 0xC0, 0xFF}));
    CHECK(encode(AsioSampleType::Int32LSB24, 1.5f, 4) == bytes({0xFF, 0xFF, 0x7F, 0x00}));
    CHECK(encode(AsioSampleType::Float32LSB, 1.5f, 4) == bytes({0x00, 0x00, 0x80, 0x3F}));
}

TEST_CASE("a stride touches only its own channel", "[io]") {
    const auto in = bytes({0x00, 0x40, 0x00, 0xC0});   // Int16LSB: 0.5, -0.5
    std::vector<float> inter(4, 9.0f);
    asioToFloat(in.data(), AsioSampleType::Int16LSB, inter.data() + 1, 2, 2);
    CHECK(inter == std::vector<float>{9.0f, 0.5f, 9.0f, -0.5f});

    const std::vector<float> src{0.1f, 0.5f, 0.2f, -0.5f};
    std::vector<unsigned char> out(4, 0);
    floatToAsio(src.data() + 1, 2, out.data(), AsioSampleType::Int16LSB, 2);
    CHECK(out == bytes({0x00, 0x40, 0x00, 0xC0}));
}

TEST_CASE("an unsupported type reads as silence", "[io]") {
    CHECK(decode(AsioSampleType::Int32MSB, bytes({0x40, 0x00, 0x00, 0x00})) == 0.0f);
}
