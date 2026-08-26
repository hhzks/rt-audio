# Fails if a platform header appears in a layer that is supposed to be portable.
# Wire this into CI. One grep is worth a hundred code review comments.

set(PORTABLE_DIRS src/core src/dsp src/engine)
set(FORBIDDEN
    "windows.h" "Windows.h" "mmdeviceapi.h" "audioclient.h" "avrt.h"
    "objbase.h" "combaseapi.h" "asio.h"
    "alsa/asoundlib.h" "jack/jack.h" "pulse/"
    "CoreAudio/" "AudioToolbox/")

set(VIOLATIONS "")
foreach(dir ${PORTABLE_DIRS})
    file(GLOB_RECURSE files "${dir}/*.h" "${dir}/*.cpp")
    foreach(file ${files})
        file(READ "${file}" contents)
        foreach(bad ${FORBIDDEN})
            if(contents MATCHES "#[ \t]*include[ \t]*[<\"]${bad}")
                list(APPEND VIOLATIONS "${file} includes ${bad}")
            endif()
        endforeach()
    endforeach()
endforeach()

if(VIOLATIONS)
    message("Layering violations:")
    foreach(v ${VIOLATIONS})
        message("  ${v}")
    endforeach()
    message(FATAL_ERROR "Platform headers must only appear under src/io/")
endif()

message(STATUS "Layering OK: core/dsp/engine are platform-free")
