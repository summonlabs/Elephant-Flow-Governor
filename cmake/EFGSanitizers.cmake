# Sanitizer configuration.
#
# On MSVC the AddressSanitizer runtime is provided by the Visual Studio
# installation; /fsanitize=address is supported and exercised by the EFG
# validation scripts. UndefinedBehaviorSanitizer is not available from MSVC,
# so it is applied only for GNU/Clang toolchains and reported as such.

function(efg_apply_sanitizers target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
        target_link_options(${target} PRIVATE /INCREMENTAL:NO)
        message(STATUS "EFG: AddressSanitizer enabled for target ${target} (MSVC)")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        message(STATUS "EFG: AddressSanitizer + UndefinedBehaviorSanitizer enabled for target ${target}")
    else()
        message(WARNING "EFG: no sanitizer configuration known for compiler ${CMAKE_CXX_COMPILER_ID}")
    endif()
endfunction()
