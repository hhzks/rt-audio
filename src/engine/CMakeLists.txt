add_library(rt_engine STATIC AudioEngine.cpp LoopbackProbe.cpp LatencyAnalyzer.cpp LatencyRun.cpp)
target_link_libraries(rt_engine PUBLIC rt_dsp rt_core)
rt_set_warnings(rt_engine)
