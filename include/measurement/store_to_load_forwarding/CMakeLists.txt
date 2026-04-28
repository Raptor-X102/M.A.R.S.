add_library(mars_store_to_load_forwarding INTERFACE)
add_library(mars::store_to_load_forwarding ALIAS mars_store_to_load_forwarding)

target_include_directories(mars_store_to_load_forwarding
    INTERFACE
        $<BUILD_INTERFACE:${MARS_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(mars_store_to_load_forwarding
    INTERFACE
        mars::core
        mars::platform
        mars::infra_logging
)
