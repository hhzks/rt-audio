#pragma once
#include <cstddef>
#include <cstdint>

namespace rt {

using Frame   = std::int64_t;   // absolute sample position on the stream timeline
using FrameCount = int;         // block sizes stay in int; they are always small

inline constexpr int kMaxChannels   = 8;
inline constexpr int kMaxBlockFrames = 2048;
inline constexpr int kMaxEffects     = 15;   // RT_MAX_STRIPS minus the master strip

// Audio code indexes with ints (channels, frames) but containers want size_t.
// This makes each conversion explicit and greppable instead of turning off
// -Wsign-conversion, which is how real bugs get to hide.
inline constexpr std::size_t idx(int i) noexcept { return static_cast<std::size_t>(i); }
inline constexpr std::size_t idx(long long i) noexcept { return static_cast<std::size_t>(i); }

} // namespace rt
