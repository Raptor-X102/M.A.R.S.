add_library(mars_tlb INTERFACE)
add_library(mars::tlb ALIAS mars_tlb)

target_include_directories(mars_tlb
    INTERFACE
        $<BUILD_INTERFACE:${MARS_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(mars_tlb
    INTERFACE
        mars::options
        mars::sanitizers
        mars::warnings
        mars::core
        mars::platform
        mars::infra_logging
)

if(MARS_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
