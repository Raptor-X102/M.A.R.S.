add_library(mars_rob STATIC
    rob_measurer.cpp
)
add_library(mars::rob ALIAS mars_rob)

target_link_libraries(mars_rob
    PUBLIC
        mars_platform
        mars_core
        #mars_cache          # rob зависит от cache
        mars_options
    PRIVATE
        mars_warnings
        asmjit::asmjit
)

mars_apply_target_defaults(mars_rob)

if(MARS_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
