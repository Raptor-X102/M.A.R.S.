add_library(mars_return_address_stack INTERFACE)
add_library(mars::return_address_stack ALIAS mars_return_address_stack)

target_include_directories(mars_return_address_stack
    INTERFACE
        $<BUILD_INTERFACE:${MARS_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(mars_return_address_stack
    INTERFACE
        mars::core
        mars::platform
        mars::infra_logging
)
