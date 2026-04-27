mars_add_test(mars_tlb_tests
    tlb_measurer_table_test.cpp
)

target_link_libraries(mars_tlb_tests
    PRIVATE
        mars::tlb
        mars::infra_logging
)
