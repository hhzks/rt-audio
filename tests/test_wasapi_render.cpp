#include "io/wasapi/WasapiRender.h"
#include "io/wasapi/WasapiError.h"
#include <catch2/catch_test_macros.hpp>

using namespace rt;

namespace {

struct FakePadding {
    std::uint32_t padding = 0;
    std::int32_t  hr      = 0;
    int           calls   = 0;
    std::int32_t operator()(std::uint32_t& out) { ++calls; out = padding; return hr; }
};

} // namespace

TEST_CASE("exclusive takes the whole buffer without a padding query", "[io]") {
    FakePadding q{ 144, 0, 0 };
    const auto r = wasapiRenderFrames(true, 144, q);
    CHECK(r.frames == 144u);
    CHECK(q.calls == 0);
}

TEST_CASE("shared writes only the free space", "[io]") {
    FakePadding q{ 576, 0, 0 };
    const auto r = wasapiRenderFrames(false, 1056, q);
    CHECK(r.frames == 480u);
    CHECK(q.calls == 1);
}

TEST_CASE("shared with a full buffer writes nothing", "[io]") {
    FakePadding q{ 1056, 0, 0 };
    CHECK(wasapiRenderFrames(false, 1056, q).frames == 0u);
}

TEST_CASE("a failed padding query returns its HRESULT and no frames", "[io]") {
    FakePadding q{ 0, kHrDeviceInvalidated, 0 };
    const auto r = wasapiRenderFrames(false, 1056, q);
    CHECK(r.hr == kHrDeviceInvalidated);
    CHECK(r.frames == 0u);
}

TEST_CASE("exclusive capture reads one buffer per capture event and none on render events", "[io]") {
    CHECK(wasapiCaptureReads(true, true) == 1);
    CHECK(wasapiCaptureReads(true, false) == 0);
}

TEST_CASE("shared capture drains every packet on both events", "[io]") {
    CHECK(wasapiCaptureReads(false, true) == kCaptureDrainAll);
    CHECK(wasapiCaptureReads(false, false) == kCaptureDrainAll);
}
