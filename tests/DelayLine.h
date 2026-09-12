#pragma once
#include "core/Types.h"

#include <cstddef>
#include <vector>

namespace rt::testing {

// Returns what was written `delayFrames` frames earlier. read() before write() for each block.
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

} // namespace rt::testing
