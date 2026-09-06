#include "engine/LoopbackProbe.h"
#include "engine/LatencyAnalyzer.h"
#include "TestHarness.h"

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

void testProbeAndAnalyzerAgreeOnKnownDelay() {
    SweepConfig cfg;
    cfg.seconds = 0.1; cfg.primeSeconds = 0.02;
    cfg.maxLatencySeconds = 0.05; cfg.tailSeconds = 0.02;

    constexpr int D = 512;
    LoopbackProbe probe;
    probe.prepare(48000.0, 128, 2, cfg);
    probe.arm();

    DelayLine loop(D, 2);
    std::vector<float> in(128 * 2, 0.0f), out(128 * 2, 0.0f);
    int guard = 0;
    while (probe.state() != ProbeState::Done && guard++ < 100000) {
        loop.read(in.data(), 128);
        probe.process(in.data(), out.data(), 128);
        loop.write(out.data(), 128);
    }
    CHECK(probe.state() == ProbeState::Done);

    const auto r = analyzeLatency(probe.reference(), probe.captured(),
                                  static_cast<int>(cfg.maxLatencySeconds * 48000.0), 48000.0);
    CHECK(r.valid);
    CHECK_NEAR(r.lagFrames, D, 0.5);
}

void testIdleAndDoneEmitSilence() {
    SweepConfig cfg;
    LoopbackProbe probe;
    probe.prepare(48000.0, 128, 2, cfg);
    std::vector<float> in(128 * 2, 0.0f), out(128 * 2, 7.0f);
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
    TEST_MAIN_END
}
