#include "io/DeviceFactory.h"
#include "io/null/NullDevice.h"
#include <stdexcept>

#if defined(_WIN32)
  #include "io/wasapi/WasapiDevice.h"
#endif

namespace rt {

const char* backendName(Backend b) {
    switch (b) {
    case Backend::Wasapi: return "WASAPI";
    case Backend::Asio:   return "ASIO";
    case Backend::Alsa:   return "ALSA";
    case Backend::Null:   return "Null";
    default:              return "Default";
    }
}

std::vector<std::string> availableBackends() {
    std::vector<std::string> v;
#if defined(_WIN32)
    v.emplace_back("wasapi");
#endif
    v.emplace_back("null");
    return v;
}

std::unique_ptr<IAudioDevice> createAudioDevice(Backend backend) {
    if (backend == Backend::Null) return std::make_unique<NullDevice>();

#if defined(_WIN32)
    if (backend == Backend::Default || backend == Backend::Wasapi)
        return std::make_unique<WasapiDevice>();
    if (backend == Backend::Asio)
        throw std::runtime_error(
            "ASIO backend not implemented. Steinberg's SDK cannot be redistributed; "
            "download it separately and add src/io/asio/AsioDevice.cpp.");
#else
    if (backend == Backend::Default) return std::make_unique<NullDevice>();
    if (backend == Backend::Alsa)
        throw std::runtime_error("ALSA backend not implemented yet -- see src/io/alsa/");
#endif

    throw std::runtime_error(std::string("backend not available on this platform: ")
                             + backendName(backend));
}

} // namespace rt
