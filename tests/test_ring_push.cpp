#include "core/RingPush.h"
#include "TestHarness.h"

#include <cstddef>
#include <vector>

using namespace rt;

namespace {

constexpr std::size_t kCh = 2;

// channel 0 positive, channel 1 negative, so a split interleave is visible.
std::vector<float> makeBlock(std::size_t frames, float tag = 0.0f) {
    std::vector<float> b(frames * kCh);
    for (std::size_t f = 0; f < frames; ++f) {
        b[f * kCh]     =  (static_cast<float>(f) + 1.0f) + tag;
        b[f * kCh + 1] = -(static_cast<float>(f) + 1.0f) - tag;
    }
    return b;
}

bool interleaveIntact(const std::vector<float>& v) {
    for (std::size_t i = 0; i + 1 < v.size(); i += kCh)
        if (!(v[i] > 0.0f && v[i + 1] < 0.0f)) return false;
    return true;
}

std::vector<float> drain(SpscRingBuffer& ring) {
    std::vector<float> out(ring.readAvailable());
    if (!out.empty()) ring.pop(out.data(), out.size());
    return out;
}

} // namespace

void testEmptyRingTakesExactlyTheBlock() {
    SpscRingBuffer ring;
    ring.reset(192 * kCh * 4);

    const auto block = makeBlock(192);
    const bool evicted = pushEvictingOldest(ring, block.data(), block.size(), kCh);

    CHECK(!evicted);
    CHECK(ring.readAvailable() == block.size());
    CHECK(drain(ring) == block);
}

void testRepeatedBlocksWithRoomNeverEvict() {
    SpscRingBuffer ring;
    ring.reset(192 * kCh * 4);

    for (int i = 0; i < 3; ++i) {
        const auto b = makeBlock(192, static_cast<float>(i) * 1000.0f);
        CHECK(!pushEvictingOldest(ring, b.data(), b.size(), kCh));
    }
    CHECK(ring.readAvailable() == 192 * kCh * 3);
    CHECK(interleaveIntact(drain(ring)));
}

void testOverflowEvictsAndKeepsFrameAlignment() {
    SpscRingBuffer ring;
    ring.reset(192 * kCh * 4);

    bool sawEviction = false;
    for (int i = 0; i < 6; ++i) {
        const auto b = makeBlock(192, static_cast<float>(i) * 1000.0f);
        sawEviction |= pushEvictingOldest(ring, b.data(), b.size(), kCh);
    }

    CHECK(sawEviction);
    CHECK(ring.readAvailable() % kCh == 0);
    CHECK(interleaveIntact(drain(ring)));
}

void testOversizedBlockKeepsNewestFrames() {
    SpscRingBuffer ring;
    ring.reset(64);

    const auto block = makeBlock(128);
    pushEvictingOldest(ring, block.data(), block.size(), kCh);

    CHECK(ring.readAvailable() % kCh == 0);
    CHECK(ring.readAvailable() < ring.capacity());

    const auto out = drain(ring);
    CHECK(interleaveIntact(out));
    if (out.size() >= kCh)
        CHECK_NEAR(out[out.size() - kCh], block[block.size() - kCh], 0.0);
}

void testNeverExceedsCapacity() {
    SpscRingBuffer ring;
    ring.reset(192 * kCh * 4);

    for (int i = 0; i < 20; ++i) {
        const auto b = makeBlock(192, static_cast<float>(i));
        pushEvictingOldest(ring, b.data(), b.size(), kCh);
        CHECK(ring.readAvailable() < ring.capacity());
    }
}

int main() {
    RUN(testEmptyRingTakesExactlyTheBlock);
    RUN(testRepeatedBlocksWithRoomNeverEvict);
    RUN(testOverflowEvictsAndKeepsFrameAlignment);
    RUN(testOversizedBlockKeepsNewestFrames);
    RUN(testNeverExceedsCapacity);
    TEST_MAIN_END
}
