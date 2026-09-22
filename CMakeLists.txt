cmake_minimum_required(VERSION 3.25)
project(rt_audio LANGUAGES CXX VERSION 0.1.0)

set(RT_MIN_GCC 16.2)
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS RT_MIN_GCC)
    message(FATAL_ERROR
        "The mingw branch needs GCC ${RT_MIN_GCC} or later (C++26 contracts and std::inplace_vector).\n"
        "Found: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER}).\n"
        "Windows: MSYS2, pacman -Syu and pacman -S mingw-w64-x86_64-gcc, then put <msys64>\\mingw64\\bin first on PATH\n"
        "or use a CMakeUserPresets.json. Linux: GCC ${RT_MIN_GCC} or later, for example the gcc:16.2.0 image.")
endif()
add_compile_options(-fcontracts)
add_link_options(-fcontracts)
add_compile_options(-fcontract-evaluation-semantic=$<IF:$<CONFIG:Debug>,enforce,observe>)
add_compile_definitions($<$<NOT:$<CONFIG:Debug>>:RT_CONTRACTS_OBSERVE>)
if(MINGW)
    add_link_options(-static)
    set(Rust_CARGO_TARGET x86_64-pc-windows-gnu CACHE STRING "Rust target for MinGW builds")
endif()

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
option(RT_BUILD_TUI "Build the rt_rig terminal UI (needs cargo)" ON)

option(RT_ASIO "Build the ASIO backend (Windows; the binaries are covered by GPLv3)" OFF)
set(RT_ASIO_SDK_DIR "" CACHE PATH "Extracted ASIOSDK folder; empty = download the pinned SDK")

if(RT_ASIO)
    if(NOT WIN32)
        message(FATAL_ERROR "rt_audio: RT_ASIO=ON works only on Windows")
    endif()
    include(cmake/AsioSdk.cmake)
    rt_resolve_asio_sdk(RT_ASIO_SDK_PATH)
    message(STATUS "rt_audio: ASIO backend enabled (SDK ${RT_ASIO_SDK_PATH}); binaries built this way are covered by GPLv3")
endif()

set(RT_TUI_ENABLED OFF)
if(RT_BUILD_TUI)
    find_program(RT_CARGO cargo HINTS "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin")
    if(RT_CARGO)
        set(RT_TUI_ENABLED ON)
        if(MSVC)
            # Rust's MSVC target always links the release DLL CRT. This must be
            # set before any target is created.
            set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
        endif()
    else()
        message(STATUS "rt_audio: cargo not found, rt_rig disabled")
    endif()
endif()

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
add_subdirectory(src/ffi)
add_subdirectory(src/app)

if(RT_TUI_ENABLED)
    # Corrosion calls the toolchain's own cargo, not the rustup proxy, so the
    # rust-toolchain.toml pin has to be passed in explicitly.
    file(STRINGS "${CMAKE_SOURCE_DIR}/src/tui/rust-toolchain.toml" _rt_channel REGEX "^channel")
    string(REGEX REPLACE "^channel *= *\"([^\"]+)\".*" "\\1" _rt_toolchain "${_rt_channel}")
    set(Rust_TOOLCHAIN "${_rt_toolchain}" CACHE STRING "Rust toolchain used by Corrosion")

    include(FetchContent)
    FetchContent_Declare(Corrosion
        GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
        GIT_TAG v0.6.1)
    FetchContent_MakeAvailable(Corrosion)
    add_subdirectory(src/tui)
endif()

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
