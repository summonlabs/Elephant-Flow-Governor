add_library(efg_test_support STATIC support/test_main.cpp)
target_link_libraries(efg_test_support PUBLIC efg::efg)
target_include_directories(efg_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_compile_features(efg_test_support PUBLIC cxx_std_20)
set_target_properties(efg_test_support PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(EFG_WARNINGS_AS_ERRORS)
    efg_apply_warnings(efg_test_support)
endif()
if(EFG_SANITIZE)
    efg_apply_sanitizers(efg_test_support)
endif()

set(EFG_UNIT_TESTS
    unit
    governance
    persistence
    concurrency
    adversarial
    property
    benchmark
)
set(EFG_CLUSTER_TESTS
    cluster
)

foreach(name IN LISTS EFG_UNIT_TESTS)
    add_executable(efg-test-${name} ${name}/main.cpp)
    target_link_libraries(efg-test-${name} PRIVATE efg_test_support)
    set_target_properties(efg-test-${name} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    if(EFG_WARNINGS_AS_ERRORS)
        efg_apply_warnings(efg-test-${name})
    endif()
    if(EFG_SANITIZE)
        efg_apply_sanitizers(efg-test-${name})
    endif()
    file(GLOB test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${name}/*.cpp")
    foreach(source IN LISTS test_sources)
        get_filename_component(stem "${source}" NAME_WE)
        if(NOT stem STREQUAL "main")
            target_sources(efg-test-${name} PRIVATE ${source})
        endif()
    endforeach()
    add_test(NAME efg.${name} COMMAND efg-test-${name})
endforeach()

# The multiprocess suite needs the coordinator and worker executables.
if(EFG_BUILD_APPS)
    foreach(name IN LISTS EFG_CLUSTER_TESTS)
        add_executable(efg-test-${name} ${name}/main.cpp)
        target_link_libraries(efg-test-${name} PRIVATE efg_test_support)
        target_compile_definitions(efg-test-${name} PRIVATE
            EFG_TEST_COORDINATOR_BINARY="$<TARGET_FILE:efg-coordinator>"
            EFG_TEST_WORKER_BINARY="$<TARGET_FILE:efg-worker>"
        )
        set_target_properties(efg-test-${name} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        if(EFG_WARNINGS_AS_ERRORS)
            efg_apply_warnings(efg-test-${name})
        endif()
        if(EFG_SANITIZE)
            efg_apply_sanitizers(efg-test-${name})
        endif()
        file(GLOB test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${name}/*.cpp")
        foreach(source IN LISTS test_sources)
            get_filename_component(stem "${source}" NAME_WE)
            if(NOT stem STREQUAL "main")
                target_sources(efg-test-${name} PRIVATE ${source})
            endif()
        endforeach()
        add_test(NAME efg.${name} COMMAND efg-test-${name})
    endforeach()
else()
    message(STATUS "EFG: multiprocess cluster tests require EFG_BUILD_APPS; skipped")
endif()
