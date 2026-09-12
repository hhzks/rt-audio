#pragma once
#include <cstdint>

namespace rt {

struct RenderFrames {
    std::int32_t  hr;
    std::uint32_t frames;
};

template <class GetPadding>
RenderFrames wasapiRenderFrames(bool exclusive, std::uint32_t bufferFrames,
                                GetPadding&& getPadding) noexcept {
    if (exclusive) return { 0, bufferFrames };
    std::uint32_t padding = 0;
    const std::int32_t hr = getPadding(padding);
    if (hr < 0) return { hr, 0 };
    return { hr, bufferFrames - padding };
}

inline constexpr int kCaptureDrainAll = INT32_MAX;

// Exclusive event-driven capture gives one buffer per capture event; any other
// read returns a buffer with no new audio on some drivers.
constexpr int wasapiCaptureReads(bool exclusive, bool captureEvent) noexcept {
    if (!exclusive) return kCaptureDrainAll;
    return captureEvent ? 1 : 0;
}

constexpr std::uint32_t wasapiRingTargetFrames(std::uint32_t captureBufferFrames,
                                               std::uint32_t renderBufferFrames) noexcept {
    return 2 * (captureBufferFrames > renderBufferFrames ? captureBufferFrames : renderBufferFrames);
}

} // namespace rt
