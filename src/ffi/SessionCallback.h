#pragma once
#include "engine/AudioEngine.h"
#include "engine/LoopbackProbe.h"
#include "io/IAudioDevice.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace rt {

enum class CallbackMode : std::uint8_t { Normal, Silent, ProbeDirect, ProbeChain };

class SessionCallback : public IAudioCallback {
public:
    SessionCallback(AudioEngine& engine, LoopbackProbe& probe) : engine_(engine), probe_(probe) {}

    void setMode(CallbackMode m) noexcept { mode_.store(m, std::memory_order_release); }
    CallbackMode mode() const noexcept { return mode_.load(std::memory_order_acquire); }

    void audioDeviceProcess(const float* in, float* out, FrameCount n) noexcept override {
        switch (mode_.load(std::memory_order_acquire)) {
        case CallbackMode::Normal:
            engine_.processInterleaved(in, out, n);
            return;
        case CallbackMode::Silent:
            std::memset(out, 0, sizeof(float) * idx(n) * idx(engine_.numChannels()));
            engine_.monitor(in, out, n);
            return;
        case CallbackMode::ProbeDirect:
            probe_.process(in, out, n);
            engine_.monitor(in, out, n);
            return;
        case CallbackMode::ProbeChain:
            probe_.process(in, out, n);
            engine_.processInterleaved(out, out, n);
            return;
        }
    }

private:
    AudioEngine&              engine_;
    LoopbackProbe&            probe_;
    std::atomic<CallbackMode> mode_{CallbackMode::Normal};
};

} // namespace rt
