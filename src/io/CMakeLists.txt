set(RT_IO_SOURCES
    DeviceFactory.cpp
    null/NullDevice.cpp
)

if(WIN32)
    list(APPEND RT_IO_SOURCES wasapi/WasapiDevice.cpp)
endif()

add_library(rt_io STATIC ${RT_IO_SOURCES})
target_link_libraries(rt_io PUBLIC rt_engine)

# PRIVATE is deliberate: these must not leak up into rt_dsp's link line.
if(WIN32)
    target_link_libraries(rt_io PRIVATE ole32 avrt uuid)
endif()
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(rt_io PRIVATE Threads::Threads)
endif()

rt_set_warnings(rt_io)
