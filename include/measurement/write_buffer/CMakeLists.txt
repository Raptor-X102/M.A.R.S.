add_library(mars_write_buffer INTERFACE)
add_library(mars::write_buffer ALIAS mars_write_buffer)

target_include_directories(mars_write_buffer
    INTERFACE
        $<BUILD_INTERFACE:${MARS_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(mars_write_buffer
    INTERFACE
        mars::core
        mars::platform
        mars::infra_logging
)
