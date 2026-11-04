#include "dsp/EffectChain.h"

int main() {
    rt::EffectChain chain;
    chain.reset();
    return static_cast<int>(chain.size());
}
