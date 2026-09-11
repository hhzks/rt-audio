cmake_minimum_required(VERSION 3.25)
project(rt_audio LANGUAGES CXX VERSION 0.1.0)

# CMake < 4.1 has no C++26 mapping for clang-cl; /std:c++latest is what 4.1 emits.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang"
   AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 17.0
   AND NOT CMAKE_CXX26_STANDARD_COMPILE_OPTION)
    set(CMAKE_CXX26_STANDARD_COMPILE_OPTION  "-std:c++latest")
    set(CMAKE_CXX26_EXTENSION_COMPILE_OPTION "-std:c++latest")
    set(CMAKE_CXX_STANDARD_LATEST 26)
    list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
         CMAKE_CXX26_STANDARD_COMPILE_OPTION CMAKE_CXX26_EXTENSION_COMPILE_OPTION)
    message(STATUS "rt_audio: back-filled clang-cl C++26 mapping "
                   "(CMake ${CMAKE_VERSION} predates it)")
endif()

set(RT_CXX_STANDARD 26)

if(CMAKE_CXX_STANDARD_LATEST LESS RT_CXX_STANDARD)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        set(_rt_hint "MSVC has no /std:c++26 in any release. Use clang-cl:\n"
                     "    winget install LLVM.LLVM\n"
                     "    cmake --preset windows-clang-cl\n")
    else()
        set(_rt_hint "Need GCC >= 14 or Clang >= 17, and CMake >= 3.25 (this CMake: ${CMAKE_VERSION})." )
    endif()
    message(FATAL_ERROR
        "rt_audio requires C++${RT_CXX_STANDARD}, but "
        "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} tops out at "
        "C++${CMAKE_CXX_STANDARD_LATEST}.\n" ${_rt_hint})
endif()

set(CMAKE_CXX_STANDARD ${RT_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

message(STATUS "rt_audio: C++${CMAKE_CXX_STANDARD} "
               "(${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}, "
               "max available: ${CMAKE_CXX_STANDARD_LATEST})")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(RT_BUILD_TESTS "Build unit tests" ON)
option(RT_BUILD_TOOLS "Build offline tools" ON)

include(cmake/Warnings.cmake)

if(MSVC)
    add_compile_options($<$<CONFIG:Release>:/Zi>)
    add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF>)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
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

# Build should fail if a platform header sneaks into the portable layers
add_custom_target(check_layering
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/cmake/CheckLayering.cmake
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking that core/dsp/engine contain no platform headers")
