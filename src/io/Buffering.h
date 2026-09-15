#pragma once
#include <cstdint>

namespace rt {

constexpr std::uint32_t ringTargetFrames(std::uint32_t blockFrames, double blocks) noexcept {
    return static_cast<std::uint32_t>(blocks * static_cast<double>(blockFrames) + 0.5);
}

constexpr double bufferedRoundTripMs(double captureFrames, double renderFrames, double ringFrames,
                                     double renderRate, double resamplerFrames,
                                     double captureRate) noexcept {
    return 1000.0 * (captureFrames + renderFrames + ringFrames) / renderRate
         + 1000.0 * resamplerFrames / captureRate;
}

} // namespace rt
