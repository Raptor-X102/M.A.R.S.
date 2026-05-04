add_library(mars_write_buffer STATIC
    write_buffer_measurer.cpp
)
add_library(mars::write_buffer ALIAS mars_write_buffer)

target_link_libraries(mars_write_buffer
    PUBLIC
        mars_platform
        mars_core
        mars_options
    PRIVATE
        mars_warnings
)

mars_apply_target_defaults(mars_write_buffer)

if(MARS_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
