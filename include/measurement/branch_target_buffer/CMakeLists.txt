add_library(mars_branch_target_buffer INTERFACE)
add_library(mars::branch_target_buffer ALIAS mars_branch_target_buffer)

target_include_directories(mars_branch_target_buffer
    INTERFACE
        $<BUILD_INTERFACE:${MARS_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(mars_branch_target_buffer
    INTERFACE
        mars::core
        mars::platform
        mars::infra_logging
)
