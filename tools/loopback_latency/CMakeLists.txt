add_executable(loopback_latency main.cpp)
target_link_libraries(loopback_latency PRIVATE rt_io rt_engine)
target_include_directories(loopback_latency PRIVATE ${CMAKE_SOURCE_DIR}/tools)
rt_set_warnings(loopback_latency)
