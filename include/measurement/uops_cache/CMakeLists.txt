add_library(mars_uops_cache INTERFACE)
add_library(mars::uops_cache ALIAS mars_uops_cache)

target_include_directories(mars_uops_cache
    INTERFACE
        $<BUILD_INTERFACE:${MARS_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(mars_uops_cache
    INTERFACE
        mars::core
        mars::platform
        mars::infra_logging
)

if(MARS_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
