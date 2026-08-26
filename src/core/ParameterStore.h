#pragma once
#include <atomic>

namespace rt {

// The UI/control thread writes, the audio thread reads. Never the reverse.
//
// Plain atomic<float> is correct here because each parameter is independent --
// a torn read between two of them does not matter. The moment you need several
// values to change ATOMICALLY TOGETHER (e.g. a whole filter's coefficient set),
// stop using this and post a command struct through an SpscRingBuffer instead,
// or you will get a block processed with half-old, half-new coefficients.
//
// Every consumer smooths these; see ParamSmoother.
struct ParameterStore {
    std::atomic<bool>  bypass{false};

    std::atomic<float> inputGain{1.0f};        // linear
    std::atomic<float> outputGain{1.0f};       // linear

    std::atomic<float> gateThresholdDb{-45.0f};

    std::atomic<float> drive{1.0f};            // 1.0 = clean
    std::atomic<float> mix{0.0f};              // 0 = dry, 1 = fully distorted
};

static_assert(std::atomic<float>::is_always_lock_free,
              "atomic<float> must be lock-free or the audio thread can block");

} // namespace rt
