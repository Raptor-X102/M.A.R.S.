mars_add_test(mars_bht_tests
    branch_history_table_table_test.cpp
)

target_link_libraries(mars_bht_tests
    PRIVATE
        mars::platform
        mars::core
        mars::infra_logging
)
