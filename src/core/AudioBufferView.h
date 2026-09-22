#pragma once
#include "Types.h"

namespace rt {

// Non-owning, planar (de-interleaved) view of audio.
// The engine hands one of these to every effect; effects process IN PLACE.
// No ownership, no allocation, no lifetime management. Cheap to copy.
class AudioBufferView {
public:
    AudioBufferView() = default;
    AudioBufferView(float* const* channels, int numChannels, FrameCount numFrames) noexcept
        : channels_(channels), numChannels_(numChannels), numFrames_(numFrames) {}

    float*       channel(int c)       noexcept pre(c >= 0 && c < numChannels_) { return channels_[c]; }
    const float* channel(int c) const noexcept pre(c >= 0 && c < numChannels_) { return channels_[c]; }

    int        numChannels() const noexcept { return numChannels_; }
    FrameCount numFrames()   const noexcept { return numFrames_; }

    // Same buffers, fewer frames. Used when an effect processes in sub-blocks.
    AudioBufferView withFrames(FrameCount n) const noexcept pre(n >= 0 && n <= numFrames_) {
        return AudioBufferView(channels_, numChannels_, n);
    }

private:
    float* const* channels_ = nullptr;
    int           numChannels_ = 0;
    FrameCount    numFrames_ = 0;
};

} // namespace rt
