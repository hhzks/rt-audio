#include "io/DeviceFactory.h"
#include "io/null/NullDevice.h"
#include <stdexcept>

#if defined(_WIN32)
  #include "io/wasapi/WasapiDevice.h"
#endif
#if defined(RT_HAVE_ALSA)
  #include "io/alsa/AlsaDevice.h"
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
#if defined(RT_HAVE_ALSA)
    v.emplace_back("alsa");
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
  #if defined(RT_HAVE_ALSA)
    if (backend == Backend::Default || backend == Backend::Alsa)
        return std::make_unique<AlsaDevice>();
  #else
    if (backend == Backend::Alsa)
        throw std::runtime_error(
            "ALSA backend not compiled in. Install libasound2-dev and reconfigure.");
    if (backend == Backend::Default) return std::make_unique<NullDevice>();
  #endif
#endif

    throw std::runtime_error(std::string("backend not available on this platform: ")
                             + backendName(backend));
}

} // namespace rt
