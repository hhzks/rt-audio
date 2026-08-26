cmake_minimum_required(VERSION 3.21)
project(rt_audio LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(RT_BUILD_TESTS "Build unit tests" ON)
option(RT_BUILD_TOOLS "Build offline tools" ON)

include(cmake/Warnings.cmake)

# ---------------------------------------------------------------------------
# Realtime code wants symbols in release builds. You cannot debug a dropout you
# cannot see a stack for, and /Zi + /DEBUG costs nothing at runtime.
# ---------------------------------------------------------------------------
if(MSVC)
    add_compile_options($<$<CONFIG:Release>:/Zi>)
    add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF>)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    # Do NOT enable /fp:fast globally. It lets the compiler reassociate float
    # operations, which changes IIR filter behaviour and can break stability.
    add_compile_options(/fp:precise)
endif()

add_subdirectory(src/core)
add_subdirectory(src/dsp)
add_subdirectory(src/engine)
add_subdirectory(src/io)
add_subdirectory(src/app)

if(RT_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(RT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# The architecture guard. This is what keeps the Linux port a weekend instead
# of a rewrite: it fails the build if a platform header sneaks into the
# portable layers. Run manually with: cmake --build . --target check_layering
# ---------------------------------------------------------------------------
add_custom_target(check_layering
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/cmake/CheckLayering.cmake
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking that core/dsp/engine contain no platform headers")
