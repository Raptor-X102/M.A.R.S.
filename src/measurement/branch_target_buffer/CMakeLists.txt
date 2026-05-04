add_library(mars_branch_target_buffer STATIC
    branch_target_buffer_measurer.cpp
)
add_library(mars::branch_target_buffer ALIAS mars_branch_target_buffer)

target_link_libraries(mars_branch_target_buffer
    PUBLIC
        mars_platform
        mars_core
        mars_infra_logging
        mars_options
    PRIVATE
        mars_warnings
)

mars_apply_target_defaults(mars_branch_target_buffer)

if(MARS_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
