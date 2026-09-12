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

} // namespace rt
