add_library(mars_exec_ports STATIC
    exec_ports_measurer.cpp
)
add_library(mars::exec_ports ALIAS mars_exec_ports)

target_link_libraries(mars_exec_ports
    PUBLIC
        mars_platform
        mars_core
        mars_infra_logging
        mars_options
    PRIVATE
        mars_warnings
)

mars_apply_target_defaults(mars_exec_ports)

if(MARS_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
