add_library(mars_branch_history_table INTERFACE)
add_library(mars::branch_history_table ALIAS mars_branch_history_table)

target_include_directories(mars_branch_history_table
    INTERFACE
        $<BUILD_INTERFACE:${MARS_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(mars_branch_history_table
    INTERFACE
        mars::core
        mars::platform
        mars::infra_logging
)
