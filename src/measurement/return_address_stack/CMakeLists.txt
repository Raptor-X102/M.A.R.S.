add_library(mars_return_address_stack STATIC
    return_address_stack_measurer.cpp
)
add_library(mars::return_address_stack ALIAS mars_return_address_stack)

target_link_libraries(mars_return_address_stack
    PUBLIC
        mars_platform
        mars_core
        mars_infra_logging
        mars_options
    PRIVATE
        mars_warnings
)

mars_apply_target_defaults(mars_return_address_stack)

if(MARS_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
