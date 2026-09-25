#pragma once
#include "io/IAudioDevice.h"
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rt {

enum class Backend { Default, Wasapi, Asio, Alsa, Null };

// The single place in the codebase that branches on platform.
std::unique_ptr<IAudioDevice> createAudioDevice(Backend backend = Backend::Default);

std::vector<std::string> availableBackends();
const char* backendName(Backend b);

std::optional<Backend> backendFromName(std::string_view name);

Backend          resolveBackend(Backend b);
std::string_view backendKey(Backend b);

inline constexpr std::string_view kAsioTrademarkNotice =
    "ASIO is a registered trademark of Steinberg Media Technologies GmbH.";

struct BackendCaps {
    bool ring               = false;
    bool oneDriver          = false;
    bool driverPanel        = false;
    bool exclusiveMode      = false;
    bool rateFromDevice     = false;
    bool blockZeroPreferred = false;
    bool blockRounded       = false;
    bool systemAudio        = false;
    std::string_view displayName;
    std::string_view notice;
};

BackendCaps backendCaps(Backend b);

bool        backendAvailable(std::string_view key);
std::string backendList();   // for example "wasapi | asio | null"

} // namespace rt
