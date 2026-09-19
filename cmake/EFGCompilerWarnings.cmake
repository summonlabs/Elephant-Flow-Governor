# Strict, portable warning configuration. /W4 /WX on MSVC, -Wall -Wextra -Werror elsewhere.

function(efg_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /utf-8
            /MP
            /wd4127  # conditional expression is constant (used deliberately in static_assert helpers)
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
            -Wshadow
            -Wconversion
            -Wsign-conversion
            -Wold-style-cast
            -Wcast-qual
            -Wcast-align
            -Wdouble-promotion
            -Wformat=2
            -Wimplicit-fallthrough
            -Wnull-dereference
            -Wnon-virtual-dtor
            -Woverloaded-virtual
            -Wunused
            -Wuseless-cast
            -Wzero-as-null-pointer-constant
        )
    endif()
endfunction()
