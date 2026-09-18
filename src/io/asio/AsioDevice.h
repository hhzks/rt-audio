#pragma once
#ifdef _WIN32

#include "io/IAudioDevice.h"
#include "io/asio/AsioLogic.h"
#include "io/asio/ComHostThread.h"

#include <windows.h>
#include <objbase.h>

#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rt {

// Duplex through one ASIO driver. Every IASIO call runs on host_; the driver's own thread calls
// bufferSwitch, which runs the engine callback directly. One open AsioDevice per process.
class AsioDevice : public IAudioDevice {
public:
    using DriverFactory = std::function<IASIO*(const std::string& id)>;

    AsioDevice();
    explicit AsioDevice(DriverFactory factory);
    ~AsioDevice() override;
    AsioDevice(const AsioDevice&) = delete;
    AsioDevice& operator=(const AsioDevice&) = delete;

    std::vector<DeviceInfo> enumerate() override;
    void open(const DeviceConfig& config, IAudioCallback* callback) override;
    void start() override;
    void stop() override;
    void close() override;
    DeviceStatus status() const override;
    bool isRunning() const override { return running_.load(std::memory_order_acquire); }

private:
    static void      onBufferSwitch(long index, ASIOBool directProcess);
    static ASIOTime* onBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool directProcess);
    static void      onSampleRateChanged(ASIOSampleRate rate);
    static long      onAsioMessage(long selector, long value, void* message, double* opt);

    std::string driverId(const DeviceConfig& config);
    void openOnHost(const std::string& id, const DeviceConfig& config, IAudioCallback* callback);
    void releaseOnHost() noexcept;
    void resetOnHost() noexcept;
    void refreshLatencies() noexcept;
    void requestReset() noexcept;
    void process(long index) noexcept;

    static std::atomic<AsioDevice*> active_;
    static ASIOCallbacks            callbacks_;

    DriverFactory                  factory_;
    std::unique_ptr<ComHostThread> host_;
    IASIO*                         driver_ = nullptr;
    bool                           buffersCreated_ = false;
    IAudioCallback*                callback_ = nullptr;
    DeviceConfig                   config_{};
    std::vector<ASIOBufferInfo>    buffers_;   // inputs first, then outputs
    std::vector<AsioSampleType>    types_;     // one per buffer
    int                            inputs_ = 0, outputs_ = 0;
    FrameCount                     block_ = 0;
    bool                           useOutputReady_ = false;
    std::vector<float>             engineIn_, engineOut_;
    std::atomic<bool>              running_{false};
    std::atomic<bool>              processing_{false};
    std::atomic<bool>              resetPending_{false};
    std::atomic<std::uint64_t>     xruns_{0};
    mutable std::mutex             statusMutex_;
    DeviceStatus                   status_{};
};

} // namespace rt
#endif
