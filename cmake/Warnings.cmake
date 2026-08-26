function(rt_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /WX)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow
            -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
            -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion
            -Wdouble-promotion -Wformat=2)
    endif()
endfunction()
