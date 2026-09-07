#include "engine/LatencyAnalyzer.h"
#include "core/Types.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace rt {

LatencyResult analyzeLatency(std::span<const float> reference,
                             std::span<const float> captured,
                             int maxLagFrames,
                             double sampleRate) {
    LatencyResult r;
    const std::size_t N = reference.size();
    if (N == 0 || captured.size() < N) {
        r.rejectReason = "captured shorter than reference";
        return r;
    }

    double refEnergy = 0.0;
    for (std::size_t i = 0; i < N; ++i) {
        const double v = static_cast<double>(reference[i]);
        refEnergy += v * v;
    }
    if (refEnergy <= 0.0) { r.rejectReason = "reference is silent"; return r; }

    const std::size_t maxLag = idx(std::max(maxLagFrames, 0));
    const std::size_t lagCount = std::min(maxLag + 1, captured.size() - N + 1);

    double winEnergy = 0.0;
    for (std::size_t i = 0; i < N; ++i) {
        const double v = static_cast<double>(captured[i]);
        winEnergy += v * v;
    }

    std::vector<double> corr(lagCount, 0.0);
    for (std::size_t lag = 0; lag < lagCount; ++lag) {
        double dot = 0.0;
        for (std::size_t i = 0; i < N; ++i)
            dot += static_cast<double>(reference[i]) * static_cast<double>(captured[lag + i]);
        const double denom = std::sqrt(refEnergy * winEnergy);
        corr[lag] = denom > 0.0 ? dot / denom : 0.0;

        if (lag + 1 < lagCount) {
            const double outv = static_cast<double>(captured[lag]);
            const double inv  = static_cast<double>(captured[lag + N]);
            winEnergy += inv * inv - outv * outv;
            if (winEnergy < 0.0) winEnergy = 0.0;
        }
    }

    std::size_t peak = 0;
    double best = 0.0;
    for (std::size_t lag = 0; lag < lagCount; ++lag) {
        const double a = std::fabs(corr[lag]);
        if (a > best) { best = a; peak = lag; }
    }

    const std::size_t guard = std::min(N / 4,
        std::max<std::size_t>(8, static_cast<std::size_t>(0.002 * sampleRate)));
    double sidelobe = 0.0;
    for (std::size_t lag = 0; lag < lagCount; ++lag) {
        const std::size_t d = lag > peak ? lag - peak : peak - lag;
        if (d > guard) sidelobe = std::max(sidelobe, std::fabs(corr[lag]));
    }

    r.peakCorrelation  = corr[peak];
    r.polarityInverted = corr[peak] < 0.0;
    r.peakToSidelobe   = sidelobe > 0.0 ? best / sidelobe : 1.0e9;

    double refined = static_cast<double>(peak);
    if (peak > 0 && peak + 1 < lagCount) {
        const double y0 = std::fabs(corr[peak - 1]);
        const double y1 = best;
        const double y2 = std::fabs(corr[peak + 1]);
        const double den = y0 - 2.0 * y1 + y2;
        if (std::fabs(den) > 1e-12) refined += 0.5 * (y0 - y2) / den;
    }
    r.lagFrames = refined;
    r.lagMs     = 1000.0 * refined / sampleRate;

    if (peak + 1 >= lagCount)     { r.rejectReason = "peak at search window edge"; return r; }
    if (r.peakToSidelobe < 3.0)   { r.rejectReason = "peak-to-sidelobe below 3.0"; return r; }
    if (best < 0.1)               { r.rejectReason = "correlation below 0.1";      return r; }

    r.valid = true;
    return r;
}

} // namespace rt
