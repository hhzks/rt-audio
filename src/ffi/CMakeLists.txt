add_library(rt_ffi STATIC Session.cpp rt_ffi.cpp)
target_link_libraries(rt_ffi PUBLIC rt_io rt_engine)
rt_set_warnings(rt_ffi)
