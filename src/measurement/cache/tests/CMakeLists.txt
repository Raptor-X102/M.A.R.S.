mars_add_test(mars_cache_tests
    cache_measurer_table_test.cpp
)

target_link_libraries(mars_cache_tests
    PRIVATE
        mars::cache
)
