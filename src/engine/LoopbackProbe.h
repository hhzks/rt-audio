#pragma once
#include "core/Types.h"
#include <span>
#include <vector>

namespace rt {

struct SweepConfig {
    double loHz              = 200.0;
    double hiHz              = 10000.0;
    double seconds           = 0.4;
    float  amplitude         = 0.5f;
    double fadeSeconds       = 0.005;
    double primeSeconds      = 0.1;
    double maxLatencySeconds = 0.2;
    double tailSeconds       = 0.1;
};

void generateSweep(const SweepConfig& cfg, double sampleRate, std::vector<float>& out);

} // namespace rt
