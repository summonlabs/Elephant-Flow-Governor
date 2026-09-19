cmake_minimum_required(VERSION 3.20)

project(ElephantFlowGovernor
    VERSION 1.0.0
    DESCRIPTION "Vendor-neutral C++20 runtime for generation-bound identification and governance of long-lived high-volume network flows"
    HOMEPAGE_URL "https://github.com/summonlabs/Elephant-Flow-Governor"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(EFGOptions)
include(EFGCompilerWarnings)
include(EFGSanitizers)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

if(EFG_WARNINGS_AS_ERRORS)
    add_compile_options()
endif()

# ---------------------------------------------------------------------------
# The library. Owns elephant classification and bounded governance intent only.
# ---------------------------------------------------------------------------
add_library(efg
    src/checked.cpp
    src/hash.cpp
    src/text.cpp
    src/tri.cpp
    src/identity.cpp
    src/tick.cpp
    src/flow.cpp
    src/capacity.cpp
    src/path.cpp
    src/policy.cpp
    src/classification.cpp
    src/impact.cpp
    src/intervention.cpp
    src/codec.cpp
    src/explain.cpp
    src/governor.cpp
    src/persistence.cpp
    src/transport.cpp
    src/cluster.cpp
    src/benchmark.cpp
    src/service.cpp
)
add_library(efg::efg ALIAS efg)

target_include_directories(efg
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(efg PUBLIC cxx_std_20)
set_target_properties(efg PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

if(EFG_WARNINGS_AS_ERRORS)
    efg_apply_warnings(efg)
endif()

if(WIN32)
    target_link_libraries(efg PRIVATE ws2_32)
endif()

if(EFG_SANITIZE)
    efg_apply_sanitizers(efg)
endif()

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------
if(EFG_BUILD_APPS)
    add_subdirectory(apps)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(EFG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS efg
    EXPORT EFGTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(EFG_BUILD_APPS)
    install(TARGETS efg-governor efg-coordinator efg-worker efg-bench
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT EFGTargets
    FILE EFGTargets.cmake
    NAMESPACE efg::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EFG
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EFGConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/EFGConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EFG
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/EFGConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/EFGConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/EFGConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EFG
)

install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/efg)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "Elephant Flow Governor ${PROJECT_VERSION}")
message(STATUS "  build type        : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard      : 20")
message(STATUS "  apps              : ${EFG_BUILD_APPS}")
message(STATUS "  tests             : ${EFG_BUILD_TESTS}")
message(STATUS "  sanitizers        : ${EFG_SANITIZE}")
message(STATUS "  warnings as errors: ${EFG_WARNINGS_AS_ERRORS}")
