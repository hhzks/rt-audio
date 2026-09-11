#pragma once
#include "dsp/EffectChain.h"

namespace rt {

class Biquad;
class NoiseGate;
class Waveshaper;

struct RigChain {
    Biquad*     hpf    = nullptr;
    NoiseGate*  gate   = nullptr;
    Waveshaper* shaper = nullptr;
    Biquad*     shelf  = nullptr;
};

// High-pass 80 Hz -> gate -> drive -> low shelf 200 Hz +2 dB. Gate first so
// the distortion is not amplifying noise, tone shaping last.
RigChain buildRigChain(EffectChain& chain);

} // namespace rt
