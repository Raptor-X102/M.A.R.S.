add_library(mars_cache STATIC
    cache_measurer.cpp
    #boundary_analyzer.cpp          # если есть, иначе уберите
)
add_library(mars::cache ALIAS mars_cache)

target_link_libraries(mars_cache
    PUBLIC
        mars_platform
        mars_core
        mars_infra_logging
        mars_options
    PRIVATE
        mars_warnings
)

mars_apply_target_defaults(mars_cache)

if(MARS_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
