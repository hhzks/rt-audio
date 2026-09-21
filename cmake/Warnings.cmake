function(rt_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /WX)
        # MSVC is true for clang-cl too, and /W4 does not imply these.
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE
                -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
                -Woverloaded-virtual -Wconversion -Wsign-conversion
                -Wdouble-promotion)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow
            -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
            -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion
            -Wdouble-promotion -Wformat=2 -Werror)
    endif()
endfunction()
