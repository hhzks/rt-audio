#include "engine/SystemAudioFeed.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <vector>

using namespace rt;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double     kRate   = 48000.0;
constexpr FrameCount kPacket = 480;
constexpr FrameCount kBlock  = 64;

std::vector<float> filled(FrameCount frames, int ch, float v) {
    return std::vector<float>(idx(frames) * idx(ch), v);
}

void warm(SystemAudioFeed& f, int ch) {
    const auto packet = filled(kPacket, ch, 0.25f);
    auto out = filled(kBlock, ch, 0.0f);
    for (int i = 0; i < 4; ++i) f.push(packet.data(), kPacket);
    f.pull(out.data(), kBlock, 1.0f);
    f.pull(out.data(), kBlock, 1.0f);
    REQUIRE(!f.refilling());
}

struct Clock {
    double now = 0.0;
    double nextPacket = 0.0;
};

// Consumer time in frames. The producer crystal runs at (1 + drift) times the consumer rate.
void run(SystemAudioFeed& f, Clock& c, double sourceRate, double drift, double seconds,
         bool packets) {
    const FrameCount packetFrames = static_cast<FrameCount>(sourceRate / 100.0);
    const std::vector<float> packet(idx(packetFrames), 0.1f);
    std::vector<float> out(idx(kBlock));
    const double period = kRate / 100.0 / (1.0 + drift);
    const double end = c.now + seconds * kRate;
    while (c.now < end) {
        c.now += kBlock;
        while (c.nextPacket <= c.now) {
            if (packets) f.push(packet.data(), packetFrames);
            c.nextPacket += period;
        }
        std::fill(out.begin(), out.end(), 0.0f);
        f.pull(out.data(), kBlock, 1.0f);
    }
}

} // namespace

TEST_CASE("pull adds the system audio to the output", "[engine]") {
    SystemAudioFeed f;
    f.prepare(2, kRate, kBlock);
    f.beginSource(kRate, kPacket);
    warm(f, 2);

    auto out = filled(kBlock, 2, 0.5f);
    f.pull(out.data(), kBlock, 1.0f);
    for (float v : out) CHECK_THAT(v, WithinAbs(0.75, 0.01));
    CHECK(f.takePeak() > 0.2f);
    CHECK(f.takePeak() == 0.0f);
}

TEST_CASE("nothing plays before a source or before the target fill", "[engine]") {
    SystemAudioFeed f;
    f.prepare(2, kRate, kBlock);
    auto out = filled(kBlock, 2, 0.5f);
    f.pull(out.data(), kBlock, 1.0f);
    for (float v : out) CHECK(v == 0.5f);

    f.beginSource(kRate, kPacket);
    const auto packet = filled(kPacket, 2, 0.25f);
    f.push(packet.data(), kPacket);
    f.pull(out.data(), kBlock, 1.0f);
    for (float v : out) CHECK(v == 0.5f);
    CHECK(f.refilling());
    CHECK(f.gaps() == 0);
}

TEST_CASE("an empty ring gives one gap, then waits for the target again", "[engine]") {
    SystemAudioFeed f;
    f.prepare(2, kRate, kBlock);
    f.beginSource(kRate, kPacket);
    warm(f, 2);

    auto out = filled(kBlock, 2, 0.0f);
    for (int i = 0; i < 60; ++i) f.pull(out.data(), kBlock, 1.0f);
    CHECK(f.refilling());
    CHECK(f.gaps() == 1);

    const auto packet = filled(kPacket, 2, 0.25f);
    f.push(packet.data(), kPacket);
    std::fill(out.begin(), out.end(), 0.0f);
    f.pull(out.data(), kBlock, 1.0f);
    for (float v : out) CHECK(v == 0.0f);

    f.push(packet.data(), kPacket);
    f.pull(out.data(), kBlock, 1.0f);
    CHECK(!f.refilling());
    CHECK(f.gaps() == 1);
}

TEST_CASE("the gain ramps across a block and the sum clamps", "[engine]") {
    SystemAudioFeed f;
    f.prepare(1, kRate, kBlock);
    f.beginSource(kRate, kPacket);
    warm(f, 1);

    auto out = filled(kBlock, 1, 0.0f);
    f.pull(out.data(), kBlock, 0.0f);
    CHECK(out.front() > 0.2f);
    CHECK_THAT(out.back(), WithinAbs(0.0, 1e-6));
    for (std::size_t i = 1; i < out.size(); ++i) CHECK(out[i] <= out[i - 1] + 1e-6f);

    f.pull(out.data(), kBlock, 1.0f);
    std::fill(out.begin(), out.end(), 0.9f);
    f.pull(out.data(), kBlock, 1.0f);
    for (float v : out) CHECK(v == 1.0f);
}

TEST_CASE("a block larger than the prepared size is left alone", "[engine]") {
    SystemAudioFeed f;
    f.prepare(1, kRate, kBlock);
    f.beginSource(kRate, kPacket);
    warm(f, 1);
    auto out = filled(2 * kBlock, 1, 0.5f);
    f.pull(out.data(), 2 * kBlock, 1.0f);
    for (float v : out) CHECK(v == 0.5f);
}

TEST_CASE("drift of 100 ppm either way gives no gaps and no evictions", "[engine]") {
    for (const double drift : {100e-6, -100e-6}) {
        CAPTURE(drift);
        SystemAudioFeed f;
        f.prepare(1, kRate, kBlock);
        f.beginSource(kRate, kPacket);
        Clock c;
        run(f, c, kRate, drift, 1.0, true);
        REQUIRE(!f.refilling());
        const std::uint64_t gaps = f.gaps();

        run(f, c, kRate, drift, 180.0, true);
        CHECK(f.gaps() == gaps);
        CHECK(f.evictions() == 0);
    }
}

TEST_CASE("a 44.1 kHz source into a 48 kHz engine stays gap-free", "[engine]") {
    SystemAudioFeed f;
    f.prepare(1, kRate, kBlock);
    f.beginSource(44100.0, 441);
    Clock c;
    run(f, c, 44100.0, 0.0, 1.0, true);
    REQUIRE(!f.refilling());
    const std::uint64_t gaps = f.gaps();
    run(f, c, 44100.0, 0.0, 60.0, true);
    CHECK(f.gaps() == gaps);
    CHECK(f.evictions() == 0);
}

TEST_CASE("a pause keeps the learned trim", "[engine]") {
    SystemAudioFeed f;
    f.prepare(1, kRate, kBlock);
    f.beginSource(kRate, kPacket);
    Clock c;
    run(f, c, kRate, 100e-6, 60.0, true);
    const double before = f.trim();
    const std::uint64_t gaps = f.gaps();

    run(f, c, kRate, 100e-6, 5.0, false);
    CHECK(f.refilling());
    CHECK(f.gaps() == gaps + 1);
    CHECK(f.trim() == before);

    run(f, c, kRate, 100e-6, 10.0, true);
    double sum = 0.0;
    for (int i = 0; i < 50; ++i) {
        run(f, c, kRate, 100e-6, 1.0, true);
        sum += f.trim();
    }
    CHECK(f.gaps() == gaps + 1);
    CHECK(f.evictions() == 0);
    CHECK_THAT((sum / 50.0 - 1.0) * 1e6, WithinAbs(100.0, 100.0));
}

TEST_CASE("a stall that fills the ring is cut back on the next pull", "[engine]") {
    SystemAudioFeed f;
    f.prepare(1, kRate, kBlock);
    f.beginSource(kRate, kPacket);
    warm(f, 1);

    const auto packet = filled(kPacket, 1, 0.25f);
    for (int i = 0; i < 20; ++i) f.push(packet.data(), kPacket);
    CHECK(f.evictions() > 0);

    auto out = filled(kBlock, 1, 0.0f);
    int pulls = 0;
    while (!f.refilling() && pulls < 200) {
        f.pull(out.data(), kBlock, 1.0f);
        ++pulls;
    }
    CHECK(pulls <= 2 * kPacket / kBlock + 2);
}

TEST_CASE("a full ring drops new packets and keeps the read side to the consumer", "[engine]") {
    SystemAudioFeed f;
    f.prepare(1, kRate, kBlock);
    f.beginSource(kRate, kPacket);
    warm(f, 1);

    const auto oldPacket = filled(kPacket, 1, 0.25f);
    const auto newPacket = filled(kPacket, 1, 0.75f);
    for (int i = 0; i < 20; ++i) f.push(oldPacket.data(), kPacket);
    for (int i = 0; i < 5; ++i) f.push(newPacket.data(), kPacket);

    auto out = filled(kBlock, 1, 0.0f);
    float loudest = 0.0f;
    for (int i = 0; i < 200 && !f.refilling(); ++i) {
        std::fill(out.begin(), out.end(), 0.0f);
        f.pull(out.data(), kBlock, 1.0f);
        for (float v : out) loudest = std::max(loudest, v);
    }
    CHECK(loudest < 0.5f);
}
