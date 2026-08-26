add_library(rt_dsp STATIC
    EffectChain.cpp
    Biquad.cpp
    Waveshaper.cpp
    NoiseGate.cpp
)
target_link_libraries(rt_dsp PUBLIC rt_core)
rt_set_warnings(rt_dsp)
