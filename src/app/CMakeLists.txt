add_executable(rt_audio main.cpp)
target_link_libraries(rt_audio PRIVATE rt_io rt_engine)
rt_set_warnings(rt_audio)
