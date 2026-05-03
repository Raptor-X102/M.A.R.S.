add_library(mars_app STATIC
    application.cpp
    config_loader.cpp
)
add_library(mars::app ALIAS mars_app)

mars_apply_target_defaults(mars_app)

target_link_libraries(mars_app
    PUBLIC
        mars::cache
        mars::core
        mars::infra_logging
        mars::tlb
        mars::rob
        mars::branch_history_table
        mars::return_address_stack
        mars::exec_ports
        mars::uops_cache
        mars::branch_target_buffer
        mars::store_to_load_forwarding
        mars::write_buffer
        CLI11::CLI11
    PRIVATE
        ${MARS_YAMLCPP_TARGET}
        asmjit::asmjit
)

if(MARS_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
