#include "io/null/NullDevice.h"
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace rt {

NullDevice::~NullDevice() { close(); }

std::vector<DeviceInfo> NullDevice::enumerate() {
    DeviceInfo info;
    info.id = "null";
    info.name = "Null device (synthetic 440 Hz tone in, discard out)";
    info.maxInputChannels = info.maxOutputChannels = 2;
    info.defaultSampleRate = 48000.0;
    info.isDefaultInput = info.isDefaultOutput = true;
    return { info };
}

void NullDevice::open(const DeviceConfig& config, IAudioCallback* callback) {
    if (!callback) throw std::invalid_argument("NullDevice::open: null callback");
    config_   = config;
    callback_ = callback;

    if (config_.blockFrames <= 0) config_.blockFrames = 256;

    const auto n = idx(config_.blockFrames) * idx(config_.numChannels);
    inBuffer_.assign(n, 0.0f);
    outBuffer_.assign(n, 0.0f);

    status_.sampleRate  = config_.sampleRate;
    status_.blockFrames = config_.blockFrames;
    status_.numChannels = config_.numChannels;
    status_.backendName = "Null";
    status_.inputName   = "synthetic tone";
    status_.outputName  = "discard";
    status_.estimatedRoundTripMs =
        2.0 * 1000.0 * config_.blockFrames / config_.sampleRate;
}

void NullDevice::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&NullDevice::threadMain, this);
}

void NullDevice::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void NullDevice::close() { stop(); callback_ = nullptr; }

void NullDevice::threadMain() {
    using clock = std::chrono::steady_clock;
    const auto blockDuration = std::chrono::nanoseconds(
        static_cast<long long>(1e9 * config_.blockFrames / config_.sampleRate));
    auto nextDeadline = clock::now();

    const double phaseInc = 2.0 * 3.14159265358979323846 * 440.0 / config_.sampleRate;
    const int ch = config_.numChannels;

    while (running_.load(std::memory_order_acquire)) {
        // Generate a tone plus a little noise, so the gate has something to do.
        for (FrameCount i = 0; i < config_.blockFrames; ++i) {
            const auto s = static_cast<float>(0.25 * std::sin(tonePhase_));
            tonePhase_ += phaseInc;
            if (tonePhase_ > 6.283185307179586) tonePhase_ -= 6.283185307179586;
            for (int c = 0; c < ch; ++c) inBuffer_[idx(i * ch + c)] = s;
        }

        callback_->audioDeviceProcess(inBuffer_.data(), outBuffer_.data(), config_.blockFrames);

        nextDeadline += blockDuration;
        std::this_thread::sleep_until(nextDeadline);
    }
}

} // namespace rt
