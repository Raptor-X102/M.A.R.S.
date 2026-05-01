mars_add_test(mars_rob_tests
    rob_table_test.cpp
)

target_link_libraries(mars_rob_tests
    PRIVATE
        mars::rob
        mars::infra_logging
)
