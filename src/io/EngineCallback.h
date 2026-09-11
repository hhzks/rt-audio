#pragma once
#include "engine/AudioEngine.h"
#include "io/IAudioDevice.h"

namespace rt {

// Adapts AudioEngine to the device callback interface. This tiny class is the
// entire coupling between the I/O layer and the DSP layer.
class EngineCallback : public IAudioCallback {
public:
    explicit EngineCallback(AudioEngine& engine) : engine_(engine) {}
    void audioDeviceProcess(const float* in, float* out, FrameCount frames) noexcept override {
        engine_.processInterleaved(in, out, frames);
    }
private:
    AudioEngine& engine_;
};

} // namespace rt
