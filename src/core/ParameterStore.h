#pragma once
#include <atomic>

namespace rt {

// Engine-level controls. The UI/control thread writes, the audio thread reads.
// Per-effect parameters live in each effect's ParamBlock.
struct ParameterStore {
    std::atomic<bool>  bypass{false};

    std::atomic<float> inputGain{1.0f};        // linear
    std::atomic<float> outputGain{1.0f};       // linear
};

static_assert(std::atomic<float>::is_always_lock_free,
              "atomic<float> must be lock-free or the audio thread can block");

} // namespace rt
