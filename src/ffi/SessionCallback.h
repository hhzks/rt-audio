#pragma once
#include "engine/AudioEngine.h"
#include "engine/LoopbackProbe.h"
#include "io/IAudioDevice.h"
#include "io/ISystemAudioTap.h"

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

    void setSystemTap(ISystemAudioTap* tap) noexcept { tap_.store(tap, std::memory_order_release); }
    void setSystemGain(float gain) noexcept { gain_.store(gain, std::memory_order_relaxed); }

    void audioDeviceProcess(const float* in, float* out, FrameCount n) noexcept override {
        switch (mode_.load(std::memory_order_acquire)) {
        case CallbackMode::Normal:
            engine_.processInterleaved(in, out, n);
            if (ISystemAudioTap* tap = tap_.load(std::memory_order_acquire))
                tap->pull(out, n, gain_.load(std::memory_order_relaxed));
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
    AudioEngine&                  engine_;
    LoopbackProbe&                probe_;
    std::atomic<CallbackMode>     mode_{CallbackMode::Normal};
    std::atomic<ISystemAudioTap*> tap_{nullptr};
    std::atomic<float>            gain_{1.0f};
};

} // namespace rt
