#pragma once
#include "ffi/rt_ffi.h"
#include "core/ParamInfo.h"
#include "dsp/RigChain.h"
#include "engine/AudioEngine.h"
#include "io/DeviceFactory.h"
#include "engine/LoopbackProbe.h"
#include "ffi/SessionCallback.h"
#include "io/IAudioDevice.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace rt {

class SessionStateError : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

class SessionArgError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

enum class ReconfigureResult { Applied = 0, RolledBack = 1, Stopped = 2 };

// One caller thread only. Strip 0 is Master; strips 1..n are the rig chain.
class Session {
public:
    using DeviceMaker = std::function<std::unique_ptr<IAudioDevice>(Backend)>;

    Session();
    explicit Session(DeviceMaker make);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void open(Backend backend, const DeviceConfig& config);
    ReconfigureResult reconfigure(const DeviceConfig& next);
    std::vector<DeviceInfo> enumerate();
    void stop() noexcept;
    bool isOpen() const noexcept { return opened_; }

    void latencyEnter();
    void latencyLeave();
    bool latencyMode() const noexcept { return latencyMode_.load(std::memory_order_relaxed); }

    const DeviceConfig& config() const;
    Backend             backend() const;
    const std::string&  reconfigureMessage() const noexcept { return message_; }

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
    void checkOpened() const;
    void applyMaster() noexcept;
    std::unique_ptr<IAudioDevice> makeAndStart(const DeviceConfig& config);
    std::optional<std::string>    tryStart(const DeviceConfig& config);
    void destroyDevice() noexcept;

    AudioEngine                   engine_;      // declared first, destroyed last
    RigChain                      rig_{};
    LoopbackProbe                 probe_;
    SessionCallback               callback_{engine_, probe_};
    ParamBlock<3>                 master_;
    DeviceMaker                   make_;
    Backend                       backend_ = Backend::Null;
    DeviceConfig                  config_{};
    std::string                   stopReason_;
    std::string                   message_;
    bool                          opened_ = false;
    std::atomic<bool>             latencyMode_{false};
    std::unique_ptr<IAudioDevice> device_;      // declared last, destroyed first: joins the device thread
};

} // namespace rt
