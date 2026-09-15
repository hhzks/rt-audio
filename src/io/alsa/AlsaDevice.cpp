#include "io/alsa/AlsaDevice.h"
#ifdef RT_HAVE_ALSA

#include "io/alsa/RtSchedScope.h"
#include "io/alsa/AlsaError.h"
#include "core/RingPush.h"
#include "io/Buffering.h"

#include <algorithm>
#include <chrono>
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

void AlsaDevice::openStream(snd_pcm_t*& pcm, const char* name,
                            snd_pcm_stream_t dir, unsigned latencyUs) {
    const int err = snd_pcm_open(&pcm, name, dir, 0);
    if (err < 0) {
        std::string msg = std::string("snd_pcm_open(\"") + name + "\"): " + snd_strerror(err);
        if (err == -ENOENT)
            msg += "\nNo such PCM. Run --list to see available PCM names -- --in/--out "
                   "take a raw PCM name, so a typo there is the likely cause. On a system "
                   "with no sound card (WSL2, containers), install libasound2-plugins and "
                   "add to /etc/asound.conf:\n"
                   "  pcm.!default { type pulse }\n  ctl.!default { type pulse }";
        if (err == -EBUSY)
            msg += "\nDevice is held exclusively. A sound server (PipeWire/PulseAudio) "
                   "may own it; try the \"default\" PCM instead of a hw: device.";
        throw std::runtime_error(msg);
    }

    const int perr = snd_pcm_set_params(pcm, SND_PCM_FORMAT_FLOAT_LE,
                                        SND_PCM_ACCESS_RW_INTERLEAVED,
                                        static_cast<unsigned>(config_.numChannels),
                                        static_cast<unsigned>(config_.sampleRate),
                                        1, latencyUs);
    if (perr < 0)
        throw std::runtime_error(std::string("snd_pcm_set_params(\"") + name + "\"): "
                                 + snd_strerror(perr) + " -- requested "
                                 + std::to_string(config_.numChannels) + "ch @ "
                                 + std::to_string(config_.sampleRate) + " Hz");
}

void AlsaDevice::open(const DeviceConfig& config, IAudioCallback* callback) {
    if (callback == nullptr) throw std::invalid_argument("AlsaDevice::open: null callback");
    if (!(config.sampleRate >= 8000.0 && config.sampleRate <= 768000.0))
        throw std::invalid_argument("AlsaDevice::open: sampleRate " +
                                    std::to_string(config.sampleRate) +
                                    " outside the supported 8000..768000 Hz range");
    if (config.numChannels < 1 || config.numChannels > kMaxChannels)
        throw std::invalid_argument("AlsaDevice::open: numChannels " +
                                    std::to_string(config.numChannels) +
                                    " outside the supported 1.." +
                                    std::to_string(kMaxChannels) + " range");
    if (!validRingBlocks(config.ringBlocks))
        throw std::invalid_argument("AlsaDevice::open: ringBlocks outside 1.0..2.0");
    close();

    config_   = config;
    callback_ = callback;
    if (config_.blockFrames <= 0) config_.blockFrames = 256;

    // set_params derives period = buffer/4, so ask for 4 blocks of total latency
    // to land on the requested period size.
    const auto latencyUs = static_cast<unsigned>(
        4.0 * 1e6 * config_.blockFrames / config_.sampleRate);

    openStream(capture_, config_.inputId.empty()  ? "default" : config_.inputId.c_str(),
               SND_PCM_STREAM_CAPTURE, latencyUs);
    openStream(render_, config_.outputId.empty() ? "default" : config_.outputId.c_str(),
               SND_PCM_STREAM_PLAYBACK, latencyUs);

    snd_pcm_uframes_t capBuf = 0, capPeriod = 0, renBuf = 0, renPeriod = 0;
    snd_pcm_get_params(capture_, &capBuf, &capPeriod);
    snd_pcm_get_params(render_,  &renBuf, &renPeriod);

    const auto period = static_cast<FrameCount>(std::min(capPeriod, renPeriod));
    if (period <= 0 || period > kMaxBlockFrames)
        throw std::runtime_error("ALSA granted a period of " + std::to_string(period)
                                 + " frames, outside the engine limit of 1..."
                                 + std::to_string(kMaxBlockFrames));

    renderBufferFrames_ = renBuf;

    const auto maxBlock  = idx(period);
    const auto engineCh  = idx(config_.numChannels);

    nominalRatio_ = 1.0;
    captureRing_.reset(maxBlock * engineCh * 4);
    resampler_.prepare(config_.numChannels, nominalRatio_);
    ringTargetFrames_ = ringTargetFrames(static_cast<std::uint32_t>(period), config_.ringBlocks);
    drift_.prepare(ringTargetFrames_ * engineCh, 0.002);

    engineIn_.assign(maxBlock * engineCh, 0.0f);
    engineOut_.assign(maxBlock * engineCh, 0.0f);
    captureScratch_.assign(maxBlock * engineCh, 0.0f);
    resampleScratch_.assign(
        idx(AsyncResampler::maxOutputFor(period, nominalRatio_)) * engineCh, 0.0f);

    status_ = DeviceStatus{};
    status_.sampleRate  = config_.sampleRate;
    status_.blockFrames = period;
    status_.numChannels = config_.numChannels;
    status_.backendName = "ALSA";
    status_.inputName   = config_.inputId.empty()  ? "default" : config_.inputId;
    status_.outputName  = config_.outputId.empty() ? "default" : config_.outputId;
    status_.estimatedRoundTripMs = bufferedRoundTripMs(
        static_cast<double>(capBuf), static_cast<double>(renBuf),
        static_cast<double>(ringTargetFrames_), config_.sampleRate,
        static_cast<double>(AsyncResampler::latencyFrames()), config_.sampleRate);

    ready_.store(true, std::memory_order_release);
}

void AlsaDevice::start() {
    if (running_.load(std::memory_order_acquire)) return;
    if (thread_.joinable()) stop();

    if (!ready_.load(std::memory_order_acquire))
        throw std::runtime_error("AlsaDevice::start: device not open");

    threadReady_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&AlsaDevice::threadMain, this);

    for (int i = 0; i < 100 && !threadReady_.load(std::memory_order_acquire); ++i) {
        if (!running_.load(std::memory_order_acquire)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!running_.load(std::memory_order_acquire)) {
        if (thread_.joinable()) thread_.join();
        threadReady_.store(false, std::memory_order_relaxed);
        throw std::runtime_error("AlsaDevice::start: audio thread exited immediately "
                                 "(snd_pcm_prepare failed -- device disconnected, in an "
                                 "unexpected state, or held by another process)");
    }
}

void AlsaDevice::stop() {
    running_.store(false, std::memory_order_release);
    if (capture_ != nullptr) snd_pcm_drop(capture_);
    if (render_  != nullptr) snd_pcm_drop(render_);
    if (thread_.joinable()) thread_.join();
    threadReady_.store(false, std::memory_order_relaxed);
}

void AlsaDevice::close() {
    ready_.store(false, std::memory_order_release);
    stop();
    if (capture_ != nullptr) { snd_pcm_close(capture_); capture_ = nullptr; }
    if (render_  != nullptr) { snd_pcm_close(render_);  render_  = nullptr; }
    callback_ = nullptr;
}

DeviceStatus AlsaDevice::status() const {
    DeviceStatus s = status_;
    s.captureOverruns = captureOverruns_.load(std::memory_order_relaxed);
    s.xruns = xruns_.load(std::memory_order_relaxed);
    if (threadReady_.load(std::memory_order_acquire))
        s.backendName = schedElevated_.load(std::memory_order_relaxed)
                      ? "ALSA (SCHED_FIFO)" : "ALSA (normal priority)";
    const int err = lastErr_.load(std::memory_order_relaxed);
    if (err != 0) s.lastError = std::string("ALSA transfer failed: ") + snd_strerror(err);
    return s;
}

bool AlsaDevice::handleTransfer(snd_pcm_t* pcm, int err) noexcept {
    if (!running_.load(std::memory_order_acquire)) return false;
    switch (alsaActionFor(err)) {
    case AlsaAction::None:
    case AlsaAction::Retry:
        return true;
    case AlsaAction::Recover:
        xruns_.fetch_add(1, std::memory_order_relaxed);
        return snd_pcm_recover(pcm, err, 1) >= 0;
    case AlsaAction::Fail:
        lastErr_.store(err, std::memory_order_relaxed);
        return false;
    }
    return false;
}

void AlsaDevice::drainCapture(FrameCount frames) noexcept {
    const std::size_t ch = idx(config_.numChannels);

    resampler_.setRatio(nominalRatio_ * drift_.update(captureRing_.readAvailable()));

    const FrameCount produced =
        resampler_.process(captureScratch_.data(), frames, resampleScratch_.data(),
                           static_cast<FrameCount>(resampleScratch_.size() / ch));

    if (pushEvictingOldest(captureRing_, resampleScratch_.data(), idx(produced) * ch, ch))
        captureOverruns_.fetch_add(1, std::memory_order_relaxed);
}

void AlsaDevice::fillRender(FrameCount frames) noexcept {
    captureRing_.popOrZero(engineIn_.data(), idx(frames) * idx(config_.numChannels));
    callback_->audioDeviceProcess(engineIn_.data(), engineOut_.data(), frames);
}

void AlsaDevice::threadMain() {
    RtSchedScope sched;
    schedElevated_.store(sched.ok(), std::memory_order_relaxed);

    const FrameCount period = status_.blockFrames;
    const auto uperiod = static_cast<snd_pcm_uframes_t>(period);

    bool prepared = snd_pcm_prepare(capture_) >= 0 && snd_pcm_prepare(render_) >= 0;
    threadReady_.store(true, std::memory_order_release);

    if (prepared) {
        std::fill(engineOut_.begin(), engineOut_.end(), 0.0f);
        snd_pcm_uframes_t queued = 0;
        while (prepared && queued < renderBufferFrames_) {
            const auto chunk = std::min<snd_pcm_uframes_t>(uperiod, renderBufferFrames_ - queued);
            const snd_pcm_sframes_t w = snd_pcm_writei(render_, engineOut_.data(), chunk);
            if (w < 0) {
                if (!handleTransfer(render_, static_cast<int>(w))) { prepared = false; break; }
                continue;
            }
            queued += static_cast<snd_pcm_uframes_t>(w);
        }
        if (prepared && snd_pcm_state(render_) == SND_PCM_STATE_PREPARED) snd_pcm_start(render_);
    }

    while (prepared && running_.load(std::memory_order_acquire)) {
        const snd_pcm_sframes_t r = snd_pcm_readi(capture_, captureScratch_.data(), uperiod);
        if (r < 0) {
            if (!handleTransfer(capture_, static_cast<int>(r))) break;
            continue;
        }

        drainCapture(static_cast<FrameCount>(r));
        fillRender(period);

        const snd_pcm_sframes_t w = snd_pcm_writei(render_, engineOut_.data(), uperiod);
        if (w < 0 && !handleTransfer(render_, static_cast<int>(w))) break;
    }

    snd_pcm_drop(capture_);
    snd_pcm_drop(render_);
    running_.store(false, std::memory_order_release);
}

} // namespace rt
#endif
