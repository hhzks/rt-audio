#pragma once
#include "io/ISystemAudioTap.h"

#include <cstdint>
#include <string>

namespace rt {

enum class TapAction : std::uint8_t { Open, SameDevice };

inline TapAction tapPlan(const TapInputs& inputs, const std::string& resolvedSourceId,
                         const std::string& resolvedOutputId) {
    if (inputs.backend == Backend::Wasapi && !resolvedSourceId.empty()
        && resolvedSourceId == resolvedOutputId)
        return TapAction::SameDevice;
    return TapAction::Open;
}

} // namespace rt
