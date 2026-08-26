# Header-only: nothing to compile, but we still make a target so that
# dependency edges (and include paths) are explicit in the graph.
add_library(rt_core INTERFACE)
target_include_directories(rt_core INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(rt_core INTERFACE cxx_std_20)
