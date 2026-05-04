add_library(mars_tlb STATIC
    tlb_measurer.cpp
)
add_library(mars::tlb ALIAS mars_tlb)

target_link_libraries(mars_tlb
    PUBLIC
        mars_platform
        mars_core
        mars_infra_logging
        mars_options
    PRIVATE
        mars_warnings
)

mars_apply_target_defaults(mars_tlb)

if(MARS_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
