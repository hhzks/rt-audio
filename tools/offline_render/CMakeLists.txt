add_executable(offline_render main.cpp)
target_link_libraries(offline_render PRIVATE rt_engine rt_dsp rt_core)
rt_set_warnings(offline_render)
