add_library(mars_return_address_stack STATIC
    return_address_stack_measurer.cpp
)

add_library(mars::return_address_stack ALIAS mars_return_address_stack)

target_compile_options(mars_return_address_stack PRIVATE
    -O2
    -fno-optimize-sibling-calls
    -fno-inline-functions-called-once
    -fno-omit-frame-pointer
)

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
