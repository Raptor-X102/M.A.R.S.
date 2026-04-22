add_library(mars_infra_logging INTERFACE)
add_library(mars::infra_logging ALIAS mars_infra_logging)

target_include_directories(mars_infra_logging
    INTERFACE
        $<BUILD_INTERFACE:${MARS_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(mars_infra_logging
    INTERFACE
        mars::options
        mars::sanitizers
        mars::warnings
        spdlog::spdlog
)

if(MARS_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
