#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>

namespace rt {

// Interleaved sample formats a driver may hand us. WASAPI exclusive mode and
// ALSA both commonly use the integer ones; shared-mode WASAPI gives Float32.
enum class SampleFormat { Int16, Int24, Int32, Float32 };

constexpr int bytesPerSample(SampleFormat f) noexcept {
    switch (f) {
        case SampleFormat::Int16:   return 2;
        case SampleFormat::Int24:   return 3;
        case SampleFormat::Int32:   return 4;
        case SampleFormat::Float32: return 4;
    }
    return 0;
}

namespace detail {

constexpr float kPeak16 = 32768.0f;
constexpr float kPeak24 = 8388608.0f;
constexpr float kPeak32 = 2147483648.0f;

inline std::int32_t readInt24(const unsigned char* p) noexcept {
    auto v = static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0])
                                     | (static_cast<std::uint32_t>(p[1]) << 8)
                                     | (static_cast<std::uint32_t>(p[2]) << 16));
    if (v & 0x800000) v |= static_cast<std::int32_t>(0xFF000000u);
    return v;
}

inline void writeInt24(unsigned char* p, std::int32_t v) noexcept {
    const auto u = static_cast<std::uint32_t>(v);
    p[0] = static_cast<unsigned char>(u & 0xFFu);
    p[1] = static_cast<unsigned char>((u >> 8) & 0xFFu);
    p[2] = static_cast<unsigned char>((u >> 16) & 0xFFu);
}

// Scale a float sample to a signed integer whose full-scale magnitude is `peak`.
inline std::int32_t floatToInt(float v, float peak, std::int32_t lo, std::int32_t hi) noexcept {
    const float scaled = v * peak;
    if (scaled <= static_cast<float>(lo)) return lo;
    if (scaled >= static_cast<float>(hi)) return hi;
    return static_cast<std::int32_t>(std::lround(scaled));
}

} // namespace detail

// `count` is samples, not frames; the stride applies to the float side. src and dst must not overlap.
inline void toFloat(const void* src, float* dst, std::size_t count, SampleFormat fmt,
                    std::size_t dstStride = 1) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(src);
    switch (fmt) {
        case SampleFormat::Float32:
            if (dstStride == 1) {
                std::memcpy(dst, src, count * sizeof(float));
                return;
            }
            for (std::size_t i = 0; i < count; ++i)
                std::memcpy(dst + i * dstStride, bytes + i * 4, sizeof(float));
            return;
        case SampleFormat::Int16:
            for (std::size_t i = 0; i < count; ++i) {
                std::int16_t s;
                std::memcpy(&s, bytes + i * 2, sizeof(s));
                dst[i * dstStride] = static_cast<float>(s) / detail::kPeak16;
            }
            return;
        case SampleFormat::Int24:
            for (std::size_t i = 0; i < count; ++i)
                dst[i * dstStride] = static_cast<float>(detail::readInt24(bytes + i * 3)) / detail::kPeak24;
            return;
        case SampleFormat::Int32:
            for (std::size_t i = 0; i < count; ++i) {
                std::int32_t s;
                std::memcpy(&s, bytes + i * 4, sizeof(s));
                dst[i * dstStride] = static_cast<float>(s) / detail::kPeak32;
            }
            return;
    }
}

inline void fromFloat(const float* src, void* dst, std::size_t count, SampleFormat fmt,
                      std::size_t srcStride = 1) noexcept {
    auto* bytes = static_cast<unsigned char*>(dst);
    switch (fmt) {
        case SampleFormat::Float32:
            for (std::size_t i = 0; i < count; ++i) {
                const float v = std::clamp(src[i * srcStride], -1.0f, 1.0f);
                std::memcpy(bytes + i * 4, &v, sizeof(v));
            }
            return;
        case SampleFormat::Int16:
            for (std::size_t i = 0; i < count; ++i) {
                const auto s = static_cast<std::int16_t>(
                    detail::floatToInt(src[i * srcStride], detail::kPeak16, -32768, 32767));
                std::memcpy(bytes + i * 2, &s, sizeof(s));
            }
            return;
        case SampleFormat::Int24:
            for (std::size_t i = 0; i < count; ++i)
                detail::writeInt24(bytes + i * 3,
                    detail::floatToInt(src[i * srcStride], detail::kPeak24, -8388608, 8388607));
            return;
        case SampleFormat::Int32:
            for (std::size_t i = 0; i < count; ++i) {
                const auto s = detail::floatToInt(src[i * srcStride], detail::kPeak32,
                                                  INT32_MIN, INT32_MAX);
                std::memcpy(bytes + i * 4, &s, sizeof(s));
            }
            return;
    }
}

} // namespace rt
