add_library(mars_branch_history_table STATIC
    branch_history_table_measurer.cpp
)
add_library(mars::branch_history_table ALIAS mars_branch_history_table)

target_link_libraries(mars_branch_history_table
    PUBLIC
        mars_platform
        mars_core
        mars_options
    PRIVATE
        mars_warnings
)

mars_apply_target_defaults(mars_branch_history_table)

if(MARS_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
