#pragma once

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace rt {

struct WavData {
    std::vector<float> samples;   // interleaved
    int    channels   = 2;
    double sampleRate = 48000.0;
};

inline void writeWav(const std::string& path, const WavData& wav) {
    const int      ch        = wav.channels;
    const auto     rate      = static_cast<std::uint32_t>(wav.sampleRate);
    const auto     dataBytes = static_cast<std::uint32_t>(wav.samples.size() * sizeof(float));
    const std::uint32_t byteRate   = rate * static_cast<std::uint32_t>(ch) * 4u;
    const auto blockAlign = static_cast<std::uint16_t>(ch * 4);

    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);

    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); };

    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16);
    u16(3);                                   // IEEE float
    u16(static_cast<std::uint16_t>(ch));
    u32(rate); u32(byteRate); u16(blockAlign); u16(32);
    f.write("data", 4); u32(dataBytes);
    f.write(reinterpret_cast<const char*>(wav.samples.data()), dataBytes);
}

} // namespace rt
