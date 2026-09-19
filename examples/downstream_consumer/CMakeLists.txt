cmake_minimum_required(VERSION 3.20)

# Independent downstream consumer.
#
# This project is deliberately not part of the Elephant Flow Governor build. It
# is configured on its own against an installed package, and the only thing it
# knows about the governor is what find_package(EFG) publishes. If the exported
# package is incomplete, this configure step fails.

project(efg_downstream_consumer
    VERSION 1.0.0
    DESCRIPTION "Independent consumer of the installed Elephant Flow Governor package"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(EFG 1.0 REQUIRED CONFIG)

if(NOT TARGET efg::efg)
    message(FATAL_ERROR "find_package(EFG) did not provide the efg::efg target")
endif()

add_executable(efg_consumer main.cpp)
target_link_libraries(efg_consumer PRIVATE efg::efg)

if(MSVC)
    target_compile_options(efg_consumer PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(efg_consumer PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
add_test(NAME consumer.classifies_and_explains COMMAND efg_consumer)
