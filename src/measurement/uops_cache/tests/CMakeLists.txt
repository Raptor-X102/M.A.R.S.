mars_add_test(mars_uops_cache_tests
    uops_cache_table_test.cpp
)

target_link_libraries(mars_uops_cache_tests
    PRIVATE
        mars::uops_cache
        mars::platform
        mars::core
        mars::infra_logging
)
