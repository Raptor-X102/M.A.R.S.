mars_add_test(mars_cache_tests
    boundary_analyzer_table_test.cpp
    cache_measurer_table_test.cpp
)

target_link_libraries(mars_cache_tests
    PRIVATE
        mars::cache
)
