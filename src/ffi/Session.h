#pragma once
#include "ffi/rt_ffi.h"
#include "core/ParamInfo.h"
#include "dsp/RigChain.h"
#include "engine/AudioEngine.h"
#include "io/DeviceFactory.h"
#include "io/EngineCallback.h"
#include "io/IAudioDevice.h"

#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>

namespace rt {

class SessionStateError : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

class SessionArgError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

// One caller thread only. Strip 0 is Master; strips 1..n are the rig chain.
class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void open(Backend backend, const DeviceConfig& config);
    void stop() noexcept;
    bool isOpen() const noexcept { return device_ != nullptr; }

    std::size_t                stripCount() const noexcept;
    const char*                stripName(std::size_t strip) const;
    std::span<const ParamInfo> stripParams(std::size_t strip) const;
    FrameCount                 stripLatency(std::size_t strip) const;
    double paramDefault(std::size_t strip, std::size_t param) const;
    double getParam(std::size_t strip, std::size_t param) const;
    void   setParam(std::size_t strip, std::size_t param, double value);

    DeviceStatus deviceStatus() const;
    void         snapshot(rt_snapshot& out);

private:
    enum : std::size_t { kInGain = 0, kOutGain = 1, kBypass = 2 };

    const IEffect& effect(std::size_t strip) const;
    void checkParam(std::size_t strip, std::size_t param) const;
    void applyMaster() noexcept;

    AudioEngine                   engine_;      // declared first, destroyed last
    RigChain                      rig_{};
    EngineCallback                callback_{engine_};
    ParamBlock<3>                 master_;
    std::unique_ptr<IAudioDevice> device_;      // declared last, destroyed first: joins the device thread
};

} // namespace rt
