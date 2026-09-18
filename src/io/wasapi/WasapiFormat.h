#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace rt {

// "24-bit in 32-bit PCM" when the container is wider than the valid bits; 0 valid bits = not stated.
inline std::string formatText(std::string_view name, int bits, int validBits) {
    if (validBits > 0 && validBits < bits)
        return std::to_string(validBits) + "-bit in " + std::string(name);
    return std::string(name);
}

struct FormatCandidate {
    int  channels  = 0;
    int  bits      = 0;   // container
    int  validBits = 0;
    bool isFloat   = false;

    bool operator==(const FormatCandidate&) const = default;
};

// Exclusive endpoints often reject the mix format. Returns the first candidate `accepts` allows,
// in preference order: the mix channel count, then stereo, then mono; the widest depth first.
template <class Accepts>
std::optional<FormatCandidate> pickExclusiveFormat(int mixChannels, Accepts&& accepts) {
    struct Depth { int bits, validBits; bool isFloat; };
    constexpr Depth depths[] = {
        { 32, 32, true }, { 32, 32, false }, { 32, 24, false }, { 24, 24, false }, { 16, 16, false },
    };
    const int channelCandidates[] = { mixChannels, 2, 1 };
    for (int c : channelCandidates) {
        if (c <= 0) continue;
        for (const Depth& d : depths) {
            const FormatCandidate f{ c, d.bits, d.validBits, d.isFloat };
            if (accepts(f)) return f;
        }
    }
    return std::nullopt;
}

} // namespace rt
