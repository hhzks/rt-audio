#include "io/alsa/AlsaDevice.h"
#ifdef RT_HAVE_ALSA

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace rt {
namespace {

std::string flatten(const char* desc) {
    std::string s(desc);
    for (auto& c : s) if (c == '\n') c = ' ';
    return s;
}

} // namespace

AlsaDevice::~AlsaDevice() { close(); }

std::vector<DeviceInfo> AlsaDevice::enumerate() {
    std::vector<DeviceInfo> devices;

    DeviceInfo def;
    def.id = "default";
    def.name = "ALSA default (system mixer or sound server)";
    def.maxInputChannels = def.maxOutputChannels = 2;
    def.defaultSampleRate = 48000.0;
    def.isDefaultInput = def.isDefaultOutput = true;
    devices.push_back(def);

    void** hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) return devices;

    for (void** h = hints; *h != nullptr; ++h) {
        char* name = snd_device_name_get_hint(*h, "NAME");
        char* desc = snd_device_name_get_hint(*h, "DESC");
        char* ioid = snd_device_name_get_hint(*h, "IOID");

        if (name != nullptr && std::strcmp(name, "default") != 0) {
            const bool inOnly  = ioid != nullptr && std::strcmp(ioid, "Input")  == 0;
            const bool outOnly = ioid != nullptr && std::strcmp(ioid, "Output") == 0;

            DeviceInfo info;
            info.id   = name;
            info.name = desc != nullptr ? flatten(desc) : name;
            info.maxInputChannels  = outOnly ? 0 : 2;
            info.maxOutputChannels = inOnly  ? 0 : 2;
            info.defaultSampleRate = 48000.0;
            devices.push_back(info);
        }

        std::free(name);
        std::free(desc);
        std::free(ioid);
    }
    snd_device_name_free_hint(hints);
    return devices;
}

void AlsaDevice::open(const DeviceConfig&, IAudioCallback*) {
    throw std::runtime_error("AlsaDevice::open not implemented yet");
}
void AlsaDevice::start() { throw std::runtime_error("AlsaDevice::start not implemented yet"); }
void AlsaDevice::stop()  {}
void AlsaDevice::close() {}
DeviceStatus AlsaDevice::status() const { return status_; }

void AlsaDevice::openStream(snd_pcm_t*&, const char*, snd_pcm_stream_t, unsigned) {}
void AlsaDevice::threadMain() {}
void AlsaDevice::drainCapture(FrameCount) noexcept {}
void AlsaDevice::fillRender(FrameCount) noexcept {}
bool AlsaDevice::handleTransfer(snd_pcm_t*, int) noexcept { return false; }

} // namespace rt
#endif
