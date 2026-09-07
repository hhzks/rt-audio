#pragma once
#include <span>
#include <string>

namespace rt {

struct LatencyResult {
    double lagFrames        = 0.0;
    double lagMs            = 0.0;
    double peakCorrelation  = 0.0;
    double peakToSidelobe   = 0.0;
    bool   polarityInverted = false;
    bool   valid            = false;
    std::string rejectReason;
};

LatencyResult analyzeLatency(std::span<const float> reference,
                             std::span<const float> captured,
                             int maxLagFrames,
                             double sampleRate);

} // namespace rt
