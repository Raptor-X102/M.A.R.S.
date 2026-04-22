add_library(mars_core INTERFACE)
add_library(mars::core ALIAS mars_core)

target_include_directories(mars_core
    INTERFACE
        $<BUILD_INTERFACE:${MARS_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(mars_core
    INTERFACE
        mars::options
        mars::sanitizers
        mars::warnings
        mars::infra_logging
)

if(MARS_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
