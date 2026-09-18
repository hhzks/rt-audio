#include "io/asio/AsioLogic.h"
#include <catch2/catch_test_macros.hpp>

#include <string>

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
