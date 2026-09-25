#pragma once
#include "core/Types.h"
#include "io/DeviceFactory.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rt {

enum class SystemAudioState : std::uint8_t { Off, Idle, Playing, SameDevice, Error };

struct SystemAudioStatus {
    SystemAudioState state = SystemAudioState::Off;
    std::string      text;
    std::uint64_t    gaps = 0, evictions = 0, discontinuities = 0;
};

struct SystemSource {
    std::string id, name;
};

struct TapInputs {
    Backend     backend   = Backend::Null;
    bool        exclusive = false;
    std::string sourceId;
    std::string outputId;
};

class ISystemAudioTap {
public:
    virtual ~ISystemAudioTap() = default;
    virtual std::vector<SystemSource> sources() = 0;
    virtual void start(const TapInputs& inputs, double engineRate, int engineChannels,
                       FrameCount maxBlock) = 0;
    virtual void stop() noexcept = 0;
    virtual void pull(float* out, FrameCount n, float gain) noexcept = 0;
    virtual float takePeak() noexcept = 0;
    virtual SystemAudioStatus status() const = 0;
};

std::unique_ptr<ISystemAudioTap> createSystemAudioTap();

} // namespace rt
