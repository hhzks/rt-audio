#pragma once
#include <cstdint>

namespace rt {

enum class WasapiAction { None, Fail };

// Values of AUDCLNT_E_DEVICE_INVALIDATED and AUDCLNT_E_SERVICE_NOT_RUNNING.
// WasapiDevice.cpp static_asserts them against the SDK.
inline constexpr std::int32_t kHrDeviceInvalidated = static_cast<std::int32_t>(0x88890004u);
inline constexpr std::int32_t kHrServiceNotRunning = static_cast<std::int32_t>(0x88890010u);

constexpr WasapiAction wasapiActionFor(std::int32_t hr) noexcept {
    if (hr == kHrDeviceInvalidated || hr == kHrServiceNotRunning) return WasapiAction::Fail;
    return WasapiAction::None;
}

} // namespace rt
