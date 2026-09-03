#pragma once
#include "core/Types.h"

#include <algorithm>
#include <cstring>

namespace rt {

// Map `srcCh` interleaved channels to `dstCh`. Mono in -> duplicate to all;
// more in than out -> take the first N; fewer -> repeat the last.
inline void remapChannels(const float* src, int srcCh, float* dst, int dstCh,
                          FrameCount frames) noexcept {
    if (srcCh == dstCh) {
        std::memcpy(dst, src, sizeof(float) * static_cast<std::size_t>(frames) * idx(dstCh));
        return;
    }
    for (FrameCount i = 0; i < frames; ++i) {
        const float* s = src + static_cast<std::size_t>(i) * idx(srcCh);
        float*       d = dst + static_cast<std::size_t>(i) * idx(dstCh);
        for (int c = 0; c < dstCh; ++c) d[c] = s[std::min(c, srcCh - 1)];
    }
}

} // namespace rt
