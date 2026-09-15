#pragma once
#include "core/Types.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rt {

// Implemented by AudioEngine's adapter. Called from the driver's realtime
// thread. `in` and `out` are interleaved, numChannels * numFrames.
class IAudioCallback {
public:
    virtual ~IAudioCallback() = default;
    virtual void audioDeviceProcess(const float* in, float* out, FrameCount numFrames) noexcept = 0;
};

struct DeviceInfo {
    std::string id;          // opaque, platform-specific; pass back to open()
    std::string name;        // human readable
    int         maxInputChannels  = 0;
    int         maxOutputChannels = 0;
    double      defaultSampleRate = 0.0;
    bool        isDefaultInput    = false;
    bool        isDefaultOutput   = false;
};

inline constexpr double kMinRingBlocks     = 1.0;
inline constexpr double kMaxRingBlocks     = 2.0;
inline constexpr double kDefaultRingBlocks = 2.0;

constexpr bool validRingBlocks(double blocks) noexcept {
    return blocks >= kMinRingBlocks && blocks <= kMaxRingBlocks;
}

struct DeviceConfig {
    std::string inputId;      // empty = system default
    std::string outputId;     // empty = system default
    double      sampleRate    = 48000.0;
    FrameCount  blockFrames   = 0;      // 0 = ask the driver for its minimum
    int         numChannels   = 2;      // what the ENGINE runs at
    bool        exclusiveMode = false;  // WASAPI exclusive; ignored elsewhere
    double      ringBlocks    = kDefaultRingBlocks;  // capture ring target, in blocks; Null ignores it

    bool operator==(const DeviceConfig&) const = default;
};

// What we actually got, which is often not what we asked for.
struct DeviceStatus {
    double     sampleRate  = 0.0;
    FrameCount blockFrames = 0;
    int        numChannels = 0;
    double     estimatedRoundTripMs = 0.0; // endpoint buffers + ring target + resampler; no USB, converters or phase gap
    std::string backendName;
    std::string inputName, outputName;

    std::uint64_t captureOverruns = 0;   // ring-full events; not engine underruns
    std::uint64_t captureUnderruns = 0;  // render found the ring short and zero-filled
    std::uint64_t xruns = 0;             // device-reported underrun/overrun recoveries
    std::string   lastError;             // set on an unrecoverable transfer error; empty otherwise
};

class IAudioDevice {
public:
    virtual ~IAudioDevice() = default;

    virtual std::vector<DeviceInfo> enumerate() = 0;

    // Throws std::runtime_error with a readable message on failure.
    virtual void open(const DeviceConfig& config, IAudioCallback* callback) = 0;
    virtual void start() = 0;
    virtual void stop()  = 0;
    virtual void close() = 0;

    virtual DeviceStatus status() const = 0;
    virtual bool isRunning() const = 0;
};

} // namespace rt
