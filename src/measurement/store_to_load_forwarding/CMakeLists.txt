add_library(mars_store_to_load_forwarding STATIC
    store_to_load_forwarding_measurer.cpp
)
add_library(mars::store_to_load_forwarding ALIAS mars_store_to_load_forwarding)

target_link_libraries(mars_store_to_load_forwarding
    PUBLIC
        mars_platform
        mars_core
        mars_infra_logging
        mars_options
    PRIVATE
        mars_warnings
)

mars_apply_target_defaults(mars_store_to_load_forwarding)

if(MARS_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
