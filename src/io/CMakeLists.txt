set(RT_IO_SOURCES
    DeviceFactory.cpp
    null/NullDevice.cpp
)

if(WIN32)
    list(APPEND RT_IO_SOURCES wasapi/WasapiDevice.cpp)
endif()

add_library(rt_io STATIC ${RT_IO_SOURCES})
target_link_libraries(rt_io PUBLIC rt_engine)

if(RT_ASIO)
    target_sources(rt_io PRIVATE asio/ComHostThread.cpp asio/AsioDevice.cpp)
    target_include_directories(rt_io SYSTEM PUBLIC "${RT_ASIO_SDK_PATH}/common")
    target_compile_definitions(rt_io PUBLIC RT_HAVE_ASIO)
    target_link_libraries(rt_io PRIVATE user32)
endif()

# PRIVATE is deliberate: these must not leak up into rt_dsp's link line.
if(WIN32)
    target_link_libraries(rt_io PRIVATE ole32 avrt uuid)
endif()
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(rt_io PRIVATE Threads::Threads)

    find_package(ALSA)
    if(ALSA_FOUND)
        target_sources(rt_io PRIVATE alsa/AlsaDevice.cpp)
        target_link_libraries(rt_io PRIVATE ALSA::ALSA)
        target_compile_definitions(rt_io PRIVATE RT_HAVE_ALSA)
        message(STATUS "rt_audio: ALSA backend enabled (${ALSA_VERSION_STRING})")
    else()
        message(STATUS "rt_audio: ALSA not found, backend disabled (install libasound2-dev)")
    endif()
endif()

rt_set_warnings(rt_io)
