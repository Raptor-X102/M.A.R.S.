add_library(mars_rob INTERFACE)
add_library(mars::rob ALIAS mars_rob)

target_include_directories(mars_rob
    INTERFACE
        $<BUILD_INTERFACE:${MARS_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(mars_rob
    INTERFACE
        mars::core
        mars::platform
        mars::cache
        asmjit::asmjit
)

if(MARS_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
