#include "dsp/RigChain.h"
#include "dsp/Biquad.h"
#include "dsp/NoiseGate.h"
#include "dsp/Waveshaper.h"

#include <memory>
#include <utility>

namespace rt {

namespace {

template <class T, class... Args>
T* addTo(EffectChain& chain, Args&&... args) {
    auto fx = std::make_unique<T>(std::forward<Args>(args)...);
    T* raw = fx.get();
    chain.add(std::move(fx));
    return raw;
}

} // namespace

RigChain buildRigChain(EffectChain& chain) {
    RigChain rig;
    rig.hpf    = addTo<Biquad>(chain, Biquad::Type::HighPass, 80.0, 0.707);
    rig.gate   = addTo<NoiseGate>(chain);
    rig.shaper = addTo<Waveshaper>(chain);
    rig.shelf  = addTo<Biquad>(chain, Biquad::Type::LowShelf, 200.0, 0.707, 2.0);
    return rig;
}

} // namespace rt
