#include "engine/LoopbackProbe.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace rt {

void generateSweep(const SweepConfig& cfg, double sampleRate, std::vector<float>& out) {
    const auto n = static_cast<std::size_t>(cfg.seconds * sampleRate);
    out.assign(n, 0.0f);
    if (n == 0) return;

    const double T = cfg.seconds;
    const double R = std::log(cfg.hiHz / cfg.loHz);
    const auto fade = static_cast<std::size_t>(cfg.fadeSeconds * sampleRate);

    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double phase = 2.0 * std::numbers::pi * cfg.loHz * T / R
                           * (std::exp(t * R / T) - 1.0);
        double w = 1.0;
        if (fade > 0) {
            if (i < fade)
                w = 0.5 * (1.0 - std::cos(std::numbers::pi * static_cast<double>(i)
                                          / static_cast<double>(fade)));
            else if (i + fade >= n)
                w = 0.5 * (1.0 - std::cos(std::numbers::pi * static_cast<double>(n - 1 - i)
                                          / static_cast<double>(fade)));
        }
        out[i] = static_cast<float>(static_cast<double>(cfg.amplitude) * std::sin(phase) * w);
    }
}

void LoopbackProbe::prepare(double sampleRate, FrameCount maxBlockFrames,
                            int numChannels, const SweepConfig& cfg) {
    generateSweep(cfg, sampleRate, sweep_);
    const auto extra = static_cast<std::size_t>(
        (cfg.maxLatencySeconds + cfg.tailSeconds) * sampleRate);
    capture_.assign(sweep_.size() + extra, 0.0f);
    primeFrames_    = static_cast<std::size_t>(cfg.primeSeconds * sampleRate);
    numChannels_    = numChannels;
    maxBlockFrames_ = maxBlockFrames;
    prepared_       = true;
    state_.store(ProbeState::Idle, std::memory_order_release);
}

void LoopbackProbe::arm() noexcept {
    sweepPos_ = capturePos_ = 0;
    primeRemaining_ = primeFrames_;
    std::fill(capture_.begin(), capture_.end(), 0.0f);
    state_.store(ProbeState::Priming, std::memory_order_release);
}

void LoopbackProbe::process(const float* in, float* out, FrameCount numFrames) noexcept {
    const std::size_t n  = idx(numFrames);
    const std::size_t ch = idx(numChannels_);
    std::memset(out, 0, sizeof(float) * n * ch);
    if (!prepared_ || numFrames > maxBlockFrames_) return;

    std::size_t f = 0;
    while (f < n) {
        switch (state_.load(std::memory_order_acquire)) {
        case ProbeState::Idle:
        case ProbeState::Done:
            return;

        case ProbeState::Priming: {
            const std::size_t take = std::min(n - f, primeRemaining_);
            primeRemaining_ -= take;
            f += take;
            if (primeRemaining_ == 0)
                state_.store(ProbeState::Emitting, std::memory_order_release);
            break;
        }

        case ProbeState::Emitting: {
            const std::size_t take = std::min({n - f,
                                               sweep_.size()   - sweepPos_,
                                               capture_.size() - capturePos_});
            for (std::size_t i = 0; i < take; ++i) {
                const float s = sweep_[sweepPos_ + i];
                for (std::size_t c = 0; c < ch; ++c) out[(f + i) * ch + c] = s;
                capture_[capturePos_ + i] = in[(f + i) * ch];
            }
            sweepPos_   += take;
            capturePos_ += take;
            f           += take;
            if (sweepPos_ == sweep_.size())
                state_.store(ProbeState::Trailing, std::memory_order_release);
            else if (take == 0) return;
            break;
        }

        case ProbeState::Trailing: {
            const std::size_t take = std::min(n - f, capture_.size() - capturePos_);
            for (std::size_t i = 0; i < take; ++i)
                capture_[capturePos_ + i] = in[(f + i) * ch];
            capturePos_ += take;
            f           += take;
            if (capturePos_ == capture_.size())
                state_.store(ProbeState::Done, std::memory_order_release);
            else if (take == 0) return;
            break;
        }
        }
    }
}

} // namespace rt
