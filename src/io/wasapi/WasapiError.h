#pragma once
#include <cstdint>
#include <format>
#include <string>

namespace rt {

enum class WasapiAction { None, Fail };

// Values of AUDCLNT_E_DEVICE_INVALIDATED, AUDCLNT_E_SERVICE_NOT_RUNNING and AUDCLNT_E_DEVICE_IN_USE.
// WasapiDevice.cpp and WasapiLoopbackTap.cpp static_assert them against the SDK.
inline constexpr std::int32_t kHrDeviceInvalidated = static_cast<std::int32_t>(0x88890004u);
inline constexpr std::int32_t kHrServiceNotRunning = static_cast<std::int32_t>(0x88890010u);
inline constexpr std::int32_t kHrDeviceInUse       = static_cast<std::int32_t>(0x8889000Au);

constexpr WasapiAction wasapiActionFor(std::int32_t hr) noexcept {
    if (hr == kHrDeviceInvalidated || hr == kHrServiceNotRunning) return WasapiAction::Fail;
    return WasapiAction::None;
}

inline std::string loopbackErrorText(std::int32_t hr, const std::string& name) {
    if (hr == kHrDeviceInvalidated) return "source removed";
    if (hr == kHrDeviceInUse)
        return name + " is in use in exclusive mode; set a different default output in Windows";
    return std::format("{}: loopback failed (hr=0x{:08x})", name, static_cast<std::uint32_t>(hr));
}

} // namespace rt
