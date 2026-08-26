#pragma once
#include "io/IAudioDevice.h"
#include <memory>
#include <string>
#include <vector>

namespace rt {

enum class Backend { Default, Wasapi, Asio, Alsa, Null };

// The single place in the codebase that branches on platform.
std::unique_ptr<IAudioDevice> createAudioDevice(Backend backend = Backend::Default);

std::vector<std::string> availableBackends();
const char* backendName(Backend b);

} // namespace rt
