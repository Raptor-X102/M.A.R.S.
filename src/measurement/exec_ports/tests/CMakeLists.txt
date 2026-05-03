mars_add_test(mars_exec_ports_tests
    exec_ports_table_test.cpp
)

target_link_libraries(mars_exec_ports_tests
    PRIVATE
        mars::exec_ports
        mars::platform
        mars::core
        mars::infra_logging
)
