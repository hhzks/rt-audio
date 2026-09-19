#pragma once
#include "core/SampleConvert.h"
#include "core/Types.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>

namespace rt {

// Values from the ASIO SDK (asio.h). AsioDevice.cpp checks them against the SDK.
enum class AsioSampleType : long {
    Int16MSB    = 0,
    Int32MSB    = 2,
    Float32MSB  = 3,
    Int16LSB    = 16,
    Int24LSB    = 17,
    Int32LSB    = 18,
    Float32LSB  = 19,
    Float64LSB  = 20,
    Int32LSB16  = 24,
    Int32LSB18  = 25,
    Int32LSB20  = 26,
    Int32LSB24  = 27,
    DSDInt8LSB1 = 32,
};

enum class AsioSelector : long {
    SelectorSupported = 1,
    EngineVersion     = 2,
    ResetRequest      = 3,
    BufferSizeChange  = 4,
    ResyncRequest     = 5,
    LatenciesChanged  = 6,
    SupportsTimeInfo  = 7,
    Overload          = 15,
};

enum class AsioAction { None, Reset, Xrun, LatenciesChanged };

struct AsioMessageReply {
    long       reply  = 0;
    AsioAction action = AsioAction::None;

    bool operator==(const AsioMessageReply&) const = default;
};

constexpr bool asioTypeSupported(AsioSampleType t) noexcept {
    switch (t) {
    case AsioSampleType::Int16LSB:
    case AsioSampleType::Int24LSB:
    case AsioSampleType::Int32LSB:
    case AsioSampleType::Float32LSB:
    case AsioSampleType::Float64LSB:
    case AsioSampleType::Int32LSB16:
    case AsioSampleType::Int32LSB18:
    case AsioSampleType::Int32LSB20:
    case AsioSampleType::Int32LSB24:
        return true;
    default:
        return false;
    }
}

constexpr const char* asioTypeName(AsioSampleType t) noexcept {
    switch (t) {
    case AsioSampleType::Int16MSB:    return "Int16MSB";
    case AsioSampleType::Int32MSB:    return "Int32MSB";
    case AsioSampleType::Float32MSB:  return "Float32MSB";
    case AsioSampleType::Int16LSB:    return "Int16LSB";
    case AsioSampleType::Int24LSB:    return "Int24LSB";
    case AsioSampleType::Int32LSB:    return "Int32LSB";
    case AsioSampleType::Float32LSB:  return "Float32LSB";
    case AsioSampleType::Float64LSB:  return "Float64LSB";
    case AsioSampleType::Int32LSB16:  return "Int32LSB16";
    case AsioSampleType::Int32LSB18:  return "Int32LSB18";
    case AsioSampleType::Int32LSB20:  return "Int32LSB20";
    case AsioSampleType::Int32LSB24:  return "Int32LSB24";
    case AsioSampleType::DSDInt8LSB1: return "DSDInt8LSB1";
    }
    return "unknown";
}

// Granularity -1: powers of two from min to max. 0: the preferred size only. A tie picks the larger size.
constexpr long chooseAsioBufferSize(long requested, long minSize, long maxSize, long preferred,
                                    long granularity) noexcept {
    if (requested <= 0 || granularity == 0) return preferred;
    const long r = requested < minSize ? minSize : (requested > maxSize ? maxSize : requested);
    const auto dist = [r](long v) { return v > r ? v - r : r - v; };
    if (granularity < 0) {
        long best = minSize;
        for (long p = 1; p > 0 && p <= maxSize; p *= 2)
            if (p >= minSize && (dist(p) < dist(best) || (dist(p) == dist(best) && p > best)))
                best = p;
        return best;
    }
    const long lo = minSize + (r - minSize) / granularity * granularity;
    const long hi = lo + granularity;
    return (hi > maxSize || dist(lo) < dist(hi)) ? lo : hi;
}

constexpr AsioMessageReply handleAsioMessage(long selector, long value) noexcept {
    switch (static_cast<AsioSelector>(selector)) {
    case AsioSelector::SelectorSupported:
        switch (static_cast<AsioSelector>(value)) {
        case AsioSelector::EngineVersion:
        case AsioSelector::ResetRequest:
        case AsioSelector::ResyncRequest:
        case AsioSelector::LatenciesChanged:
        case AsioSelector::SupportsTimeInfo:
        case AsioSelector::Overload:
            return {1, AsioAction::None};
        default:
            return {0, AsioAction::None};
        }
    case AsioSelector::EngineVersion:    return {2, AsioAction::None};
    case AsioSelector::ResetRequest:     return {1, AsioAction::Reset};
    case AsioSelector::ResyncRequest:    return {1, AsioAction::Xrun};
    case AsioSelector::Overload:         return {1, AsioAction::Xrun};
    case AsioSelector::LatenciesChanged: return {1, AsioAction::LatenciesChanged};
    case AsioSelector::SupportsTimeInfo: return {1, AsioAction::None};
    default:                             return {0, AsioAction::None};
    }
}

namespace detail {

constexpr int asioValidBits(AsioSampleType t) noexcept {
    switch (t) {
    case AsioSampleType::Int32LSB16: return 16;
    case AsioSampleType::Int32LSB18: return 18;
    case AsioSampleType::Int32LSB20: return 20;
    case AsioSampleType::Int32LSB24: return 24;
    default:                         return 32;
    }
}

constexpr float asioIntScale(int bits) noexcept {
    return static_cast<float>(1u << (bits - 1));
}

inline std::int32_t signExtend(std::uint32_t v, int bits) noexcept {
    const int shift = 32 - bits;
    return static_cast<std::int32_t>(v << shift) >> shift;
}

constexpr std::optional<SampleFormat> asioCommonFormat(AsioSampleType t) noexcept {
    switch (t) {
    case AsioSampleType::Int16LSB:   return SampleFormat::Int16;
    case AsioSampleType::Int24LSB:   return SampleFormat::Int24;
    case AsioSampleType::Int32LSB:   return SampleFormat::Int32;
    case AsioSampleType::Float32LSB: return SampleFormat::Float32;
    default:                         return std::nullopt;
    }
}

} // namespace detail

inline void asioToFloat(const void* src, AsioSampleType type, float* dst, int stride,
                        FrameCount frames) noexcept {
    if (const auto f = detail::asioCommonFormat(type)) {
        toFloat(src, dst, idx(frames), *f, idx(stride));
        return;
    }
    const auto* b = static_cast<const unsigned char*>(src);
    const std::size_t step = idx(stride);
    switch (type) {
    case AsioSampleType::Float64LSB:
        for (FrameCount i = 0; i < frames; ++i) {
            double d;
            std::memcpy(&d, b + idx(i) * 8, 8);
            dst[idx(i) * step] = static_cast<float>(d);
        }
        return;
    case AsioSampleType::Int32LSB16:
    case AsioSampleType::Int32LSB18:
    case AsioSampleType::Int32LSB20:
    case AsioSampleType::Int32LSB24: {
        const int bits = detail::asioValidBits(type);
        const float scale = detail::asioIntScale(bits);
        for (FrameCount i = 0; i < frames; ++i) {
            std::uint32_t u;
            std::memcpy(&u, b + idx(i) * 4, 4);
            dst[idx(i) * step] = static_cast<float>(detail::signExtend(u, bits)) / scale;
        }
        return;
    }
    default:
        for (FrameCount i = 0; i < frames; ++i) dst[idx(i) * step] = 0.0f;
        return;
    }
}

inline void floatToAsio(const float* src, int stride, void* dst, AsioSampleType type,
                        FrameCount frames) noexcept {
    if (const auto f = detail::asioCommonFormat(type)) {
        fromFloat(src, dst, idx(frames), *f, idx(stride));
        return;
    }
    auto* b = static_cast<unsigned char*>(dst);
    const std::size_t step = idx(stride);
    const auto at = [&](FrameCount i) { return std::clamp(src[idx(i) * step], -1.0f, 1.0f); };
    switch (type) {
    case AsioSampleType::Float64LSB:
        for (FrameCount i = 0; i < frames; ++i) {
            const double v = static_cast<double>(at(i));
            std::memcpy(b + idx(i) * 8, &v, 8);
        }
        return;
    case AsioSampleType::Int32LSB16:
    case AsioSampleType::Int32LSB18:
    case AsioSampleType::Int32LSB20:
    case AsioSampleType::Int32LSB24: {
        const int bits = detail::asioValidBits(type);
        const float scale = detail::asioIntScale(bits);
        const auto hi = static_cast<std::int32_t>((1u << (bits - 1)) - 1u);
        for (FrameCount i = 0; i < frames; ++i) {
            const std::int32_t s = detail::floatToInt(at(i), scale, -hi - 1, hi);
            std::memcpy(b + idx(i) * 4, &s, 4);
        }
        return;
    }
    default:
        return;   // open() rejects unsupported types
    }
}

} // namespace rt
