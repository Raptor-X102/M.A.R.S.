mars_add_test(mars_core_tests
    core_table_test.cpp
    summary_printer_table_test.cpp
)

target_link_libraries(mars_core_tests
    PRIVATE
        mars::core
)
