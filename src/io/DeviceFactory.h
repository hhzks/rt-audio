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

} // namespace rt
