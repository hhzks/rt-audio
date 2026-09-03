// Proves the ASRC holds unity gain across every phase, tracks its ratio without
// accumulator creep, degenerates to a pure delay at ratio 1.0, and is clean
// enough at 44.1 -> 48 to sit in a realtime capture path.
#include "dsp/AsyncResampler.h"
#include "TestHarness.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <numbers>
#include <vector>

namespace {
std::atomic<bool> g_trapArmed{false};
std::atomic<int>  g_allocations{0};
}

void* operator new(std::size_t n) {
    if (g_trapArmed.load(std::memory_order_relaxed))
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void  operator delete(void* p)                noexcept { std::free(p); }
void  operator delete[](void* p)              noexcept { std::free(p); }
void  operator delete(void* p, std::size_t)   noexcept { std::free(p); }
void  operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace rt;

namespace {

constexpr double kRatio441to48 = 44100.0 / 48000.0;

// Runs `in` (mono) through the resampler in ragged blocks, so that phase and
// history continuity across call boundaries is exercised rather than assumed.
std::vector<float> run(const std::vector<float>& in, double ratio, int blockHint) {
    AsyncResampler rs;
    rs.prepare(1, ratio);

    std::vector<float> out;
    out.reserve(in.size() * 2);

    const auto cap = idx(AsyncResampler::maxOutputFor(blockHint + 7, ratio));
    std::vector<float> scratch(cap, 0.0f);

    std::size_t pos = 0;
    int wobble = 0;
    while (pos < in.size()) {
        // Ragged on purpose: WASAPI packets are not a constant size.
        const auto want = idx(blockHint + (wobble++ % 8));
        const auto n    = static_cast<FrameCount>(std::min(want, in.size() - pos));

        const FrameCount got = rs.process(in.data() + pos, n,
                                          scratch.data(), static_cast<FrameCount>(cap));
        out.insert(out.end(), scratch.begin(), scratch.begin() + got);
        pos += idx(n);
    }
    return out;
}

// Power at `freq` versus everything else. Analysis length is chosen to hold an
// exact integer number of cycles, so rectangular-window leakage cannot pose as
// distortion at the level we are trying to measure.
double thdPlusNoiseDb(const std::vector<float>& x, std::size_t start,
                      std::size_t len, double freq, double rate) {
    double re = 0.0, im = 0.0, total = 0.0;
    for (std::size_t i = 0; i < len; ++i) {
        const double s = static_cast<double>(x[start + i]);
        const double a = 2.0 * std::numbers::pi * freq * static_cast<double>(i) / rate;
        re    += s * std::cos(a);
        im    += s * std::sin(a);
        total += s * s;
    }
    const double n = static_cast<double>(len);
    const double sig = 2.0 * (re * re + im * im) / n;   // power in the tone
    const double rest = total - sig;
    if (sig <= 0.0) return 0.0;
    return 10.0 * std::log10(std::max(rest, 1e-30) / sig);
}

} // namespace

// Every phase row must pass DC at unity, or the signal picks up a ripple at the
// rate the phase cycles -- which at 44.1/48 is an audible ~3 kHz buzz.
void testDcGainIsUnityAcrossPhases() {
    const std::vector<float> in(20000, 0.5f);
    const auto out = run(in, kRatio441to48, 128);

    CHECK(out.size() > 1000);
    for (std::size_t i = 500; i < out.size() - 500; ++i)
        CHECK_NEAR(out[i], 0.5, 1e-5);
}

void testOutputCountTracksRatio() {
    const std::vector<float> in(44100, 0.0f);
    const auto out = run(in, kRatio441to48, 160);

    const double expected = 44100.0 / kRatio441to48;   // 48000
    CHECK_NEAR(static_cast<double>(out.size()), expected, 4.0);
}

// At ratio 1.0 the only row used is phase 0, which the design collapses to a
// unit impulse: the resampler must become a pure kTaps/2 delay, not "almost".
void testUnityRatioIsPureDelay() {
    std::vector<float> in(4096);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = 0.4f * std::sin(0.037f * static_cast<float>(i));

    const auto out = run(in, 1.0, 256);
    CHECK(out.size() == in.size());

    const auto d = idx(AsyncResampler::latencyFrames());
    for (std::size_t i = 0; i + d < in.size(); ++i)
        CHECK_NEAR(out[i + d], in[i], 1e-5);
}

void testResampledSineIsClean() {
    constexpr double kIn = 44100.0, kOut = 48000.0, kFreq = 1000.0;

    std::vector<float> in(static_cast<std::size_t>(kIn) * 2);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * kFreq
                                                  * static_cast<double>(i) / kIn));

    const auto out = run(in, kIn / kOut, 128);

    // 48 samples per cycle at 48 kHz, so 48000 samples is exactly 1000 cycles.
    const double db = thdPlusNoiseDb(out, 2000, 48000, kFreq, kOut);
    std::printf("  1 kHz 44.1->48 THD+N: %.1f dB\n", db);
    CHECK(db < -80.0);
}

void testProcessDoesNotAllocate() {
    AsyncResampler rs;
    rs.prepare(2, kRatio441to48);   // allocation is allowed HERE

    constexpr FrameCount kBlock = 480;
    std::vector<float> in(idx(kBlock) * 2, 0.25f);
    std::vector<float> out(idx(AsyncResampler::maxOutputFor(kBlock, kRatio441to48)) * 2, 0.0f);

    g_allocations = 0;
    g_trapArmed = true;
    for (int i = 0; i < 200; ++i) {
        rs.setRatio(kRatio441to48 * (1.0 + 0.0001 * static_cast<double>(i % 3 - 1)));
        rs.process(in.data(), kBlock, out.data(),
                   static_cast<FrameCount>(out.size() / 2));
    }
    g_trapArmed = false;

    if (g_allocations.load() != 0)
        std::printf("  >> %d allocation(s) inside process()\n", g_allocations.load());
    CHECK(g_allocations.load() == 0);
}

int main() {
    RUN(testDcGainIsUnityAcrossPhases);
    RUN(testOutputCountTracksRatio);
    RUN(testUnityRatioIsPureDelay);
    RUN(testResampledSineIsClean);
    RUN(testProcessDoesNotAllocate);
    TEST_MAIN_END
}
