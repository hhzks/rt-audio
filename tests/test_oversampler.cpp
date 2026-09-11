// Proves the 4x polyphase FIR is transparent in the passband, reports its
// latency honestly, and actually suppresses the aliasing it exists to remove.
#include "dsp/Oversampler.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

using namespace rt;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double     kRate  = 48000.0;
constexpr FrameCount kBlock = 256;

// Runs a signal through up -> (optional nonlinearity) -> down, in blocks, so
// that filter history continuity across block boundaries is exercised too.
std::vector<float> roundTrip(const std::vector<float>& in, float drive) {
    Oversampler os;
    os.prepare();
    os.reset();

    std::vector<float> up(idx(kBlock) * idx(Oversampler::kRatio), 0.0f);
    std::vector<float> out(in.size(), 0.0f);

    for (std::size_t pos = 0; pos < in.size(); pos += idx(kBlock)) {
        const auto n = static_cast<FrameCount>(std::min(idx(kBlock), in.size() - pos));

        os.upsample(in.data() + pos, up.data(), n);

        if (drive > 0.0f)
            for (std::size_t i = 0; i < idx(n) * idx(Oversampler::kRatio); ++i)
                up[i] = std::tanh(up[i] * drive);

        os.downsample(up.data(), out.data() + pos, n);
    }
    return out;
}

// Magnitude of one frequency bin. Cheaper and clearer than pulling in an FFT
// for a test that only ever looks at a single frequency.
double goertzel(const std::vector<float>& x, std::size_t from, std::size_t count, double freq) {
    const double w    = 2.0 * std::numbers::pi * freq / kRate;
    const double cosw = std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double s0 = static_cast<double>(x[from + i]) + 2.0 * cosw * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * cosw;
    const double im = s2 * std::sin(w);
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(count);
}

std::vector<float> sine(std::size_t frames, double freq, float amplitude) {
    std::vector<float> v(frames);
    for (std::size_t i = 0; i < frames; ++i)
        v[i] = amplitude * static_cast<float>(
                   std::sin(2.0 * std::numbers::pi * freq * static_cast<double>(i) / kRate));
    return v;
}

} // namespace

TEST_CASE("DC passes through at unity gain", "[dsp]") {
    const std::vector<float> in(idx(kBlock) * 8, 1.0f);
    const auto out = roundTrip(in, 0.0f);

    // Skip the filter's fill-in ramp, then every sample must be 1.0. Reported
    // as one worst-case number so a regression is one line, not five thousand.
    double worst = 0.0;
    for (std::size_t i = idx(Oversampler::latencyFrames()) * 2; i < out.size(); ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(out[i]) - 1.0));

    CHECK_THAT(1.0 + worst, WithinAbs(1.0, 1e-4));
}

TEST_CASE("impulse peaks at the reported latency", "[dsp]") {
    std::vector<float> in(idx(kBlock) * 2, 0.0f);
    in[0] = 1.0f;

    const auto out = roundTrip(in, 0.0f);

    std::size_t peak = 0;
    for (std::size_t i = 1; i < out.size(); ++i)
        if (std::fabs(out[i]) > std::fabs(out[peak])) peak = i;

    // If this disagrees with latencyFrames(), delay compensation downstream is
    // wrong by exactly the difference, and nothing else will tell you.
    CHECK(peak == idx(Oversampler::latencyFrames()));
}

TEST_CASE("passband sine survives the round trip", "[dsp]") {
    constexpr std::size_t kFrames = 4096;
    const auto in  = sine(kFrames, 1000.0, 0.5f);
    const auto out = roundTrip(in, 0.0f);

    const auto lat = idx(Oversampler::latencyFrames());
    double worst = 0.0;
    for (std::size_t i = idx(kBlock); i + lat < kFrames; ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(out[i + lat]) -
                                          static_cast<double>(in[i])));

    CHECK_THAT(worst, WithinAbs(0.0, 2e-3));
}

TEST_CASE("oversampling suppresses the folded harmonic", "[dsp]") {
    // 15 kHz driven into saturation. The 3rd harmonic sits at 45 kHz, which is
    // above the 24 kHz Nyquist, so at base rate it folds back to 48-45 = 3 kHz
    // -- an inharmonic tone a fifth below the fundamental, plainly audible.
    // 4096 frames holds a whole number of cycles of both 15 kHz and 3 kHz, so
    // the Goertzel bins are leakage-free without windowing.
    constexpr std::size_t kAnalyse = 4096;
    constexpr std::size_t kSkip    = 256;
    constexpr double      kFund    = 15000.0;
    constexpr double      kFolded  = 3000.0;
    constexpr float       kDrive   = 6.0f;

    const auto in = sine(kAnalyse + kSkip, kFund, 0.9f);

    // Baseline: shape at base rate, exactly what Waveshaper does today.
    std::vector<float> naive(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) naive[i] = std::tanh(in[i] * kDrive);

    const auto oversampled = roundTrip(in, kDrive);

    const double naiveAlias = goertzel(naive,       kSkip, kAnalyse, kFolded);
    const double osAlias    = goertzel(oversampled, kSkip, kAnalyse, kFolded);

    // Both must still carry the fundamental -- otherwise this "improvement" is
    // just a filter that deleted the signal.
    CHECK(goertzel(oversampled, kSkip, kAnalyse, kFund) > 0.1);

    const double reductionDb = 20.0 * std::log10(naiveAlias / osAlias);
    CAPTURE(naiveAlias, osAlias);
    CHECK(reductionDb > 20.0);
}
