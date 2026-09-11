// Runs the exact same AudioEngine the live app uses, but driven from a file.

#include "engine/AudioEngine.h"
#include "dsp/Biquad.h"
#include "dsp/NoiseGate.h"
#include "dsp/Waveshaper.h"
#include "common/WavIo.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using namespace rt;

namespace {

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
        const double phase = 2.0 * std::numbers::pi * f0 * (std::exp(k * t) - 1.0) / k;
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
    double drive = 4.0, mix = 0.8, gateDb = -45.0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string{}; };
        if      (a == "--out")     outPath = next();
        else if (a == "--seconds") seconds = std::stod(next());
        else if (a == "--rate")    sampleRate = std::stod(next());
        else if (a == "--block")   block = std::stoi(next());
        else if (a == "--drive")   drive = std::stod(next());
        else if (a == "--mix")     mix = std::stod(next());
        else if (a == "--gate")    gateDb = std::stod(next());
        else { std::cerr << "unknown arg " << a << "\n"; return 1; }
    }

    try {
        AudioEngine engine;
        engine.chain().add(std::make_unique<Biquad>(Biquad::Type::HighPass, 80.0, 0.707));
        auto gate   = std::make_unique<NoiseGate>();
        auto shaper = std::make_unique<Waveshaper>();
        gate->setParam(NoiseGate::kThreshold, gateDb);
        shaper->setParam(Waveshaper::kDrive, drive);
        shaper->setParam(Waveshaper::kMix, mix);
        engine.chain().add(std::move(gate));
        engine.chain().add(std::move(shaper));
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
        const auto h = s.callbackNanos.peek();
        std::cout << "wrote " << outPath << "  (" << totalFrames << " frames)\n"
                  << "callbacks " << h.total
                  << "   p50 "   << static_cast<double>(h.nsAtPercentile(0.5))   / 1000.0 << " us"
                  << "   p99 "   << static_cast<double>(h.nsAtPercentile(0.99))  / 1000.0 << " us"
                  << "   p99.9 " << static_cast<double>(h.nsAtPercentile(0.999)) / 1000.0 << " us"
                  << "   max "   << static_cast<double>(s.peakCallbackNanos.load(std::memory_order_relaxed)) / 1000.0 << " us";
        if (h.overflow() != 0) std::cout << "   overflow " << h.overflow();
        std::cout << "\n"
                  << "deadline " << static_cast<double>(s.blockDeadlineNanos.load()) / 1000.0
                  << " us   load(p99.9) " << (s.loadFactorAt(0.999) * 100.0) << "%\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
