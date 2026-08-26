// Runs the exact same AudioEngine the live app uses, but driven from a file.
//
// This is how you should develop DSP: no hardware, no dropouts, deterministic
// output you can diff against a reference render. If a change alters the sound
// unexpectedly, you find out in a unit test rather than in your headphones
// three weeks later.

#include "engine/AudioEngine.h"
#include "dsp/Biquad.h"
#include "dsp/NoiseGate.h"
#include "dsp/Waveshaper.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace rt;

namespace {

struct WavData {
    std::vector<float> samples;   // interleaved
    int    channels   = 2;
    double sampleRate = 48000.0;
};

void writeWav(const std::string& path, const WavData& wav) {
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

// Log sweep plus broadband noise: exercises the gate, and makes aliasing from
// the un-oversampled waveshaper immediately visible in a spectrogram.
WavData makeTestSignal(double seconds, double sampleRate, int channels) {
    WavData wav;
    wav.channels = channels;
    wav.sampleRate = sampleRate;
    const auto frames = static_cast<std::size_t>(seconds * sampleRate);
    wav.samples.resize(frames * idx(channels));

    const double f0 = 40.0, f1 = 16000.0;
    const double k  = std::log(f1 / f0) / seconds;
    std::uint32_t rng = 12345;

    for (std::size_t i = 0; i < frames; ++i) {
        const double t     = static_cast<double>(i) / sampleRate;
        const double phase = 2.0 * M_PI * f0 * (std::exp(k * t) - 1.0) / k;
        rng = rng * 1664525u + 1013904223u;
        const double noise = 0.004 * ((rng >> 8) / 8388608.0 - 1.0);
        const auto   s     = static_cast<float>(0.5 * std::sin(phase) + noise);
        for (int c = 0; c < channels; ++c) wav.samples[i * idx(channels) + idx(c)] = s;
    }
    return wav;
}

} // namespace

int main(int argc, char** argv) {
    std::string outPath = "render.wav";
    double seconds = 5.0, sampleRate = 48000.0;
    FrameCount block = 256;
    int channels = 2;
    float drive = 4.0f, mix = 0.8f, gateDb = -45.0f;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string{}; };
        if      (a == "--out")     outPath = next();
        else if (a == "--seconds") seconds = std::stod(next());
        else if (a == "--rate")    sampleRate = std::stod(next());
        else if (a == "--block")   block = std::stoi(next());
        else if (a == "--drive")   drive = std::stof(next());
        else if (a == "--mix")     mix = std::stof(next());
        else if (a == "--gate")    gateDb = std::stof(next());
        else { std::cerr << "unknown arg " << a << "\n"; return 1; }
    }

    try {
        AudioEngine engine;
        engine.chain().add(std::make_unique<Biquad>(Biquad::Type::HighPass, 80.0, 0.707));
        engine.chain().add(std::make_unique<NoiseGate>(engine.params()));
        engine.chain().add(std::make_unique<Waveshaper>(engine.params()));

        engine.params().drive.store(drive);
        engine.params().mix.store(mix);
        engine.params().gateThresholdDb.store(gateDb);
        engine.prepare(sampleRate, block, channels);

        WavData input  = makeTestSignal(seconds, sampleRate, channels);
        WavData output = input;

        const auto totalFrames = input.samples.size() / idx(channels);
        for (std::size_t pos = 0; pos < totalFrames; pos += idx(block)) {
            const auto n = static_cast<FrameCount>(std::min<std::size_t>(idx(block), totalFrames - pos));
            engine.processInterleaved(input.samples.data()  + pos * idx(channels),
                                      output.samples.data() + pos * idx(channels), n);
        }

        writeWav(outPath, output);

        auto& s = engine.stats();
        std::cout << "wrote " << outPath << "  (" << totalFrames << " frames)\n"
                  << "peak callback " << static_cast<double>(s.peakCallbackNanos.load()) / 1000.0 << " us"
                  << "   deadline " << static_cast<double>(s.blockDeadlineNanos.load()) / 1000.0 << " us"
                  << "   load " << (s.loadFactor() * 100.0) << "%\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
