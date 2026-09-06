#include "engine/LoopbackProbe.h"
#include <cmath>
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
                w = 0.5 * (1.0 - std::cos(std::numbers::pi * static_cast<double>(n - i)
                                          / static_cast<double>(fade)));
        }
        out[i] = static_cast<float>(static_cast<double>(cfg.amplitude) * std::sin(phase) * w);
    }
}

} // namespace rt
