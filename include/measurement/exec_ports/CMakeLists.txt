add_library(mars_exec_ports INTERFACE)
add_library(mars::exec_ports ALIAS mars_exec_ports)

target_include_directories(mars_exec_ports
    INTERFACE
        $<BUILD_INTERFACE:${MARS_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(mars_exec_ports
    INTERFACE
        mars::core
        mars::platform
        mars::infra_logging
)
