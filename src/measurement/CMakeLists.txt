add_subdirectory(cache)
add_subdirectory(tlb)
add_subdirectory(rob)
add_subdirectory(branch_history_table)
add_subdirectory(exec_ports)
add_subdirectory(uops_cache)
add_subdirectory(return_address_stack)
add_subdirectory(branch_target_buffer)
add_subdirectory(store_to_load_forwarding)
add_subdirectory(write_buffer)

add_library(mars_measurement INTERFACE)
target_link_libraries(mars_measurement INTERFACE
    mars_cache
    mars_tlb
    mars_rob
    mars_branch_history_table
    mars_exec_ports
    mars_uops_cache
    mars_return_address_stack
    mars_branch_target_buffer
    mars_store_to_load_forwarding
    mars_write_buffer
)

if(MARS_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
