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

if(MARS_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
