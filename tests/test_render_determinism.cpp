// Guards the property that made the Linux port cheap: the DSP chain produces
// bit-identical output on every supported toolchain.
//
// This is not a "does it sound right" test -- test_biquad covers behaviour.
// This one exists so that a change to build flags or arithmetic cannot silently
// alter the rendered result on one platform and not another.
#include "engine/AudioEngine.h"
#include "dsp/Biquad.h"
#include "dsp/NoiseGate.h"
#include "dsp/Waveshaper.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <vector>

using namespace rt;

namespace {

// FNV-1a over the raw object representation of the samples.
std::uint64_t hashBits(const std::vector<float>& v) {
    std::uint64_t h = 1469598103934665603ull;
    const auto* p = reinterpret_cast<const unsigned char*>(v.data());
    for (std::size_t i = 0, n = v.size() * sizeof(float); i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// Mirrors makeTestSignal() in tools/offline_render
std::vector<float> testSignal(double seconds, double sampleRate, int channels) {
    const auto frames = static_cast<std::size_t>(seconds * sampleRate);
    std::vector<float> out(frames * idx(channels));

    const double  f0 = 40.0, f1 = 16000.0;
    const double  k   = std::log(f1 / f0) / seconds;
    std::uint32_t rng = 12345;

    for (std::size_t i = 0; i < frames; ++i) {
        const double t     = static_cast<double>(i) / sampleRate;
        const double phase = 2.0 * std::numbers::pi * f0 * (std::exp(k * t) - 1.0) / k;
        rng = rng * 1664525u + 1013904223u;
        const double noise = 0.004 * ((rng >> 8) / 8388608.0 - 1.0);
        const auto   s     = static_cast<float>(0.5 * std::sin(phase) + noise);
        for (int c = 0; c < channels; ++c) out[i * idx(channels) + idx(c)] = s;
    }
    return out;
}


constexpr std::uint64_t kExpectedHash = 0xe233a10e02dafd0full;

} // namespace

TEST_CASE("render is bit-identical across toolchains", "[engine]") {
    constexpr double     kRate     = 48000.0;
    constexpr FrameCount kBlock    = 128;
    constexpr int        kChannels = 2;
    constexpr double     kSeconds  = 1.0;

    AudioEngine engine;
    engine.chain().add(std::make_unique<Biquad>(Biquad::Type::HighPass, 80.0, 0.707));
    auto gate   = std::make_unique<NoiseGate>();
    auto shaper = std::make_unique<Waveshaper>();
    shaper->setParam(Waveshaper::kDrive, 6.0);
    shaper->setParam(Waveshaper::kMix, 0.9);
    gate->setParam(NoiseGate::kThreshold, -45.0);
    engine.chain().add(std::move(gate));
    engine.chain().add(std::move(shaper));
    engine.prepare(kRate, kBlock, kChannels);

    const std::vector<float> in  = testSignal(kSeconds, kRate, kChannels);
    std::vector<float>       out = in;

    const std::size_t totalFrames = in.size() / idx(kChannels);
    for (std::size_t pos = 0; pos < totalFrames; pos += idx(kBlock)) {
        const auto n = static_cast<FrameCount>(
            std::min<std::size_t>(idx(kBlock), totalFrames - pos));
        engine.processInterleaved(in.data()  + pos * idx(kChannels),
                                  out.data() + pos * idx(kChannels), n);
    }

    CHECK(hashBits(out) == kExpectedHash);
}
