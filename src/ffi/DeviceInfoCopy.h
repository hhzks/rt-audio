#pragma once
#include "ffi/rt_ffi.h"
#include "ffi/Utf8.h"
#include "io/IAudioDevice.h"

#include <cstdint>

namespace rt {

inline bool toDeviceInfo(const DeviceInfo& in, rt_device_info& out) noexcept {
    if (in.id.size() >= RT_ID_BYTES) return false;
    out = rt_device_info{};
    copyUtf8Truncated(out.id, sizeof out.id, in.id);
    copyUtf8Truncated(out.name, sizeof out.name, in.name);
    out.max_input_channels  = in.maxInputChannels;
    out.max_output_channels = in.maxOutputChannels;
    out.default_sample_rate = in.defaultSampleRate;
    out.is_default_input    = static_cast<std::uint8_t>(in.isDefaultInput);
    out.is_default_output   = static_cast<std::uint8_t>(in.isDefaultOutput);
    return true;
}

} // namespace rt
