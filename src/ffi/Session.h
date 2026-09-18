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
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
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

enum class LatencyKind { Control = 0, Measure = 1 };

struct LatencySettings {
    int   repeats   = 5;
    float amplitude = 0.5f;
};

struct DevicePair {
    Backend     backend = Backend::Null;
    std::string input, output;
    bool operator==(const DevicePair&) const = default;
};

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
    void controlPanel();
    bool isOpen() const noexcept { return opened_; }

    void latencyEnter();
    void latencyLeave();
    bool latencyMode() const noexcept { return latencyMode_.load(std::memory_order_relaxed); }
    void latencyStart(LatencyKind kind, const LatencySettings& settings);
    void latencyCancel() noexcept;
    void latencyStatus(rt_latency_status& out) const;

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
    void runLatency(LatencyKind kind, LatencySettings settings, std::stop_token stop);
    std::uint64_t dropoutCount();
    void drainCallbacks() noexcept;
    bool controlPassedLocked(const DeviceConfig& config) const;

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
    std::atomic<bool>             running_{false};
    mutable std::mutex            latencyMutex_;
    rt_latency_status             latency_{};
    std::vector<DevicePair>       controlPassed_;
    std::unique_ptr<IAudioDevice> device_;      // destroyed right after worker_: joins the device thread
    std::jthread                  worker_;      // declared last, destroyed first
};

} // namespace rt
