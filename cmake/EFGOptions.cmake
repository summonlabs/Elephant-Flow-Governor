# Build options for Elephant Flow Governor.

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(EFG_IS_TOP_LEVEL ON)
else()
    set(EFG_IS_TOP_LEVEL OFF)
endif()

option(EFG_BUILD_APPS "Build the Elephant Flow Governor command line applications" ${EFG_IS_TOP_LEVEL})
option(EFG_BUILD_TESTS "Build the Elephant Flow Governor test suite" ${EFG_IS_TOP_LEVEL})
option(EFG_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(EFG_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer where supported" OFF)
option(EFG_NATIVE "Build the benchmark tool with architecture native code generation" OFF)
