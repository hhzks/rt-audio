#include "io/DeviceFactory.h"
#include "io/ISystemAudioTap.h"
#include "io/null/NullDevice.h"
#include <algorithm>
#include <stdexcept>

#if defined(_WIN32)
  #include "io/wasapi/WasapiDevice.h"
  #include "io/wasapi/WasapiLoopbackTap.h"
#endif
#if defined(RT_HAVE_ASIO)
  #include "io/asio/AsioDevice.h"
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

std::optional<Backend> backendFromName(std::string_view name) {
    if (name == "wasapi")  return Backend::Wasapi;
    if (name == "asio")    return Backend::Asio;
    if (name == "alsa")    return Backend::Alsa;
    if (name == "null")    return Backend::Null;
    if (name == "default") return Backend::Default;
    return std::nullopt;
}

Backend resolveBackend(Backend b) {
    if (b != Backend::Default) return b;
#if defined(_WIN32)
    return Backend::Wasapi;
#elif defined(RT_HAVE_ALSA)
    return Backend::Alsa;
#else
    return Backend::Null;
#endif
}

std::string_view backendKey(Backend b) {
    switch (b) {
    case Backend::Wasapi:  return "wasapi";
    case Backend::Asio:    return "asio";
    case Backend::Alsa:    return "alsa";
    case Backend::Null:    return "null";
    case Backend::Default: break;
    }
    return "default";
}

#if defined(_WIN32)
constexpr bool kHasSystemTap = true;
#else
constexpr bool kHasSystemTap = false;
#endif

BackendCaps backendCaps(Backend b) {
    BackendCaps c;
    switch (resolveBackend(b)) {
    case Backend::Wasapi:
        c.ring = c.exclusiveMode = c.rateFromDevice = c.blockRounded = true;
        c.systemAudio = kHasSystemTap;
        c.displayName = "WASAPI";
        break;
    case Backend::Asio:
        c.oneDriver = c.driverPanel = c.blockZeroPreferred = c.blockRounded = true;
        c.systemAudio = kHasSystemTap;
        c.displayName = "ASIO®";
        c.notice      = kAsioTrademarkNotice;
        break;
    case Backend::Alsa:
        c.ring        = true;
        c.displayName = "ALSA";
        break;
    case Backend::Null:
    case Backend::Default:
        c.systemAudio = kHasSystemTap;
        c.displayName = "NULL";
        break;
    }
    return c;
}

std::vector<std::string> availableBackends() {
    std::vector<std::string> v;
#if defined(_WIN32)
    v.emplace_back("wasapi");
#if defined(RT_HAVE_ASIO)
    v.emplace_back("asio");
#endif
#endif
#if defined(RT_HAVE_ALSA)
    v.emplace_back("alsa");
#endif
    v.emplace_back("null");
    return v;
}

bool backendAvailable(std::string_view key) {
    const auto v = availableBackends();
    return std::ranges::find(v, key) != v.end();
}

std::string backendList() {
    std::string s;
    for (const std::string& b : availableBackends()) s += (s.empty() ? "" : " | ") + b;
    return s;
}

std::unique_ptr<IAudioDevice> createAudioDevice(Backend backend) {
    if (backend == Backend::Null) return std::make_unique<NullDevice>();

#if defined(_WIN32)
    if (backend == Backend::Default || backend == Backend::Wasapi)
        return std::make_unique<WasapiDevice>();
    if (backend == Backend::Asio)
#if defined(RT_HAVE_ASIO)
        return std::make_unique<AsioDevice>();
#else
        throw std::runtime_error("ASIO backend not built; configure with -DRT_ASIO=ON "
                                 "(binaries built this way are covered by GPLv3)");
#endif
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

std::unique_ptr<ISystemAudioTap> createSystemAudioTap() {
#if defined(_WIN32)
    try {
        return std::make_unique<WasapiLoopbackTap>();
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        return nullptr;
    }
#else
    return nullptr;
#endif
}

} // namespace rt
