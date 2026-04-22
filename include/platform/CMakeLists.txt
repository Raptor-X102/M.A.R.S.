add_library(mars_platform STATIC)
add_library(mars::platform ALIAS mars_platform)

if(UNIX AND NOT APPLE)
    target_sources(mars_platform PRIVATE
        ${CMAKE_SOURCE_DIR}/src/platform/linux/os_linux.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/pmc_linux.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/events_discovery_linux.cpp
    )
else()
    message(FATAL_ERROR "Unsupported platform: MARS currently supports Linux only")
endif()

mars_apply_target_defaults(mars_platform)

target_link_libraries(mars_platform
    PUBLIC
        mars::infra_logging
        Threads::Threads
    PRIVATE
        asmjit::asmjit
        ${PFM_LIBRARIES}
)

target_include_directories(mars_platform PRIVATE ${PFM_INCLUDE_DIRS})
