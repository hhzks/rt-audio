#include "engine/LoopbackProbe.h"
#include "engine/LatencyAnalyzer.h"
#include "TestHarness.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace rt;

namespace {
int zeroCrossings(const std::vector<float>& v, std::size_t from, std::size_t to) {
    int n = 0;
    for (std::size_t i = from + 1; i < to; ++i)
        if ((v[i - 1] < 0.0f) != (v[i] < 0.0f)) ++n;
    return n;
}

class DelayLine {
public:
    DelayLine(int delayFrames, int channels)
        : buf_(idx(delayFrames * channels), 0.0f), ch_(channels) {}

    void read(float* dst, FrameCount n) noexcept {
        for (std::size_t i = 0; i < idx(n * ch_); ++i) {
            dst[i] = buf_[pos_];
            pos_ = (pos_ + 1) % buf_.size();
        }
        pos_ = readStart_;
    }
    void write(const float* src, FrameCount n) noexcept {
        for (std::size_t i = 0; i < idx(n * ch_); ++i) {
            buf_[readStart_] = src[i];
            readStart_ = (readStart_ + 1) % buf_.size();
        }
        pos_ = readStart_;
    }
private:
    std::vector<float> buf_;
    int ch_;
    std::size_t pos_ = 0, readStart_ = 0;
};
}

void testSweepLengthAndBounds() {
    SweepConfig cfg;
    std::vector<float> s;
    generateSweep(cfg, 48000.0, s);
    CHECK(s.size() == static_cast<std::size_t>(cfg.seconds * 48000.0));
    for (float v : s) CHECK(std::fabs(v) <= cfg.amplitude + 1e-6f);
}

void testSweepFadesToZero() {
    SweepConfig cfg;
    std::vector<float> s;
    generateSweep(cfg, 48000.0, s);
    CHECK(std::fabs(s.front()) < 1e-9f);
    CHECK(std::fabs(s.back())  < 1e-9f);
}

void testSweepRisesInFrequency() {
    SweepConfig cfg;
    std::vector<float> s;
    generateSweep(cfg, 48000.0, s);
    const std::size_t tenth = s.size() / 10;
    const int early = zeroCrossings(s, tenth, 2 * tenth);
    const int late  = zeroCrossings(s, 8 * tenth, 9 * tenth);
    CHECK(late > early * 10);
}

void testStateMachineReachesDone() {
    SweepConfig cfg;
    cfg.seconds = 0.05; cfg.primeSeconds = 0.01;
    cfg.maxLatencySeconds = 0.02; cfg.tailSeconds = 0.01;

    LoopbackProbe probe;
    probe.prepare(48000.0, 128, 2, cfg);
    CHECK(probe.state() == ProbeState::Idle);
    probe.arm();
    CHECK(probe.state() == ProbeState::Priming);

    std::vector<float> in(128 * 2, 0.0f), out(128 * 2, 7.0f);
    int guard = 0;
    while (probe.state() != ProbeState::Done && guard++ < 100000)
        probe.process(in.data(), out.data(), 128);
    CHECK(probe.state() == ProbeState::Done);
    CHECK(probe.captured().size() > 0);
}

void testIrregularBlockSizes() {
    SweepConfig cfg;
    cfg.seconds = 0.05; cfg.primeSeconds = 0.01;
    cfg.maxLatencySeconds = 0.02; cfg.tailSeconds = 0.01;

    LoopbackProbe probe;
    probe.prepare(48000.0, 144, 2, cfg);
    probe.arm();

    std::vector<float> in(144 * 2, 0.0f), out(144 * 2, 0.0f);
    const FrameCount sizes[] = {144, 128, 144, 96};
    int guard = 0, k = 0;
    while (probe.state() != ProbeState::Done && guard++ < 100000)
        probe.process(in.data(), out.data(), sizes[k++ % 4]);
    CHECK(probe.state() == ProbeState::Done);
}

void runLoopbackAndCheck(const FrameCount* sizes, int nsizes) {
    SweepConfig cfg;
    cfg.seconds = 0.1; cfg.primeSeconds = 0.02;
    cfg.maxLatencySeconds = 0.05; cfg.tailSeconds = 0.02;

    constexpr int D = 512;
    LoopbackProbe probe;
    probe.prepare(48000.0, 144, 2, cfg);
    probe.arm();

    DelayLine loop(D, 2);
    std::vector<float> in(144 * 2, 0.0f), out(144 * 2, 0.0f);
    int guard = 0, k = 0, stereoChecked = 0;
    while (probe.state() != ProbeState::Done && guard++ < 100000) {
        const FrameCount n = sizes[idx(k++ % nsizes)];
        loop.read(in.data(), n);
        probe.process(in.data(), out.data(), n);
        for (FrameCount f = 0; f < n; ++f) {
            if (std::fabs(out[idx(f) * 2]) > 0.01f) {
                CHECK_NEAR(out[idx(f) * 2 + 1], out[idx(f) * 2], 1e-9);
                ++stereoChecked;
            }
        }
        loop.write(out.data(), n);
    }
    CHECK(probe.state() == ProbeState::Done);
    CHECK(stereoChecked > 1000);

    const auto r = analyzeLatency(probe.reference(), probe.captured(),
                                  static_cast<int>(cfg.maxLatencySeconds * 48000.0), 48000.0);
    CHECK(r.valid);
    CHECK_NEAR(r.lagFrames, D, 0.5);
}

void testProbeAndAnalyzerAgreeOnKnownDelay() {
    const FrameCount sizes[] = {128};
    runLoopbackAndCheck(sizes, 1);
}

void testProbeAndAnalyzerAgreeUnderIrregularBlocks() {
    const FrameCount sizes[] = {144, 128, 144, 96};
    runLoopbackAndCheck(sizes, 4);
}

void testShortSweepStillFadesOut() {
    for (double sec : {0.005, 0.008}) {
        SweepConfig cfg;
        cfg.seconds = sec;
        std::vector<float> s;
        generateSweep(cfg, 48000.0, s);
        CHECK(s.size() == static_cast<std::size_t>(sec * 48000.0));
        CHECK(std::fabs(s.front()) < 1e-9f);
        CHECK(std::fabs(s.back())  < 1e-9f);
    }
}

void testDegenerateSweepConfigProducesNothing() {
    const double los[] = {0.0, 1000.0, -200.0};
    const double his[] = {10000.0, 1000.0, 10000.0};
    for (int i = 0; i < 3; ++i) {
        SweepConfig cfg;
        cfg.loHz = los[idx(i)]; cfg.hiHz = his[idx(i)];
        std::vector<float> s;
        generateSweep(cfg, 48000.0, s);
        CHECK(s.empty());
    }
}

void testIdleAndDoneEmitSilence() {
    SweepConfig cfg;
    LoopbackProbe probe;
    probe.prepare(48000.0, 128, 2, cfg);
    std::vector<float> in(128 * 2, 0.0f), out(128 * 2, 7.0f);
    probe.process(in.data(), out.data(), 128);
    for (float v : out) CHECK_NEAR(v, 0.0, 1e-9);

    probe.arm();
    int guard = 0;
    while (probe.state() != ProbeState::Done && guard++ < 100000)
        probe.process(in.data(), out.data(), 128);
    CHECK(probe.state() == ProbeState::Done);
    std::fill(out.begin(), out.end(), 7.0f);
    probe.process(in.data(), out.data(), 128);
    for (float v : out) CHECK_NEAR(v, 0.0, 1e-9);
}

int main() {
    RUN(testSweepLengthAndBounds);
    RUN(testSweepFadesToZero);
    RUN(testSweepRisesInFrequency);
    RUN(testStateMachineReachesDone);
    RUN(testIrregularBlockSizes);
    RUN(testIdleAndDoneEmitSilence);
    RUN(testProbeAndAnalyzerAgreeOnKnownDelay);
    RUN(testProbeAndAnalyzerAgreeUnderIrregularBlocks);
    RUN(testShortSweepStillFadesOut);
    RUN(testDegenerateSweepConfigProducesNothing);
    TEST_MAIN_END
}
