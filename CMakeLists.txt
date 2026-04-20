cmake_minimum_required(VERSION 3.20)
project(MARS
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

#####################################
# Options
#####################################
option(MARS_BUILD_APPS        "Build command line applications" ON)
option(MARS_BUILD_TESTS       "Build tests" OFF)
option(MARS_ENABLE_SANITIZERS "Enable ASan/UBSan (non-MSVC)" OFF)
option(MARS_ENABLE_WERROR     "Treat warnings as errors" OFF)

#####################################
# Global settings
#####################################
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(MARS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

#####################################
# Dependencies
#####################################
find_package(CLI11 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(asmjit REQUIRED)
find_path(PFM_INCLUDE_DIRS
    NAMES perfmon/pfmlib.h
    PATH_SUFFIXES include
)
find_library(PFM_LIBRARIES
    NAMES pfm
    PATH_SUFFIXES lib
)
if(NOT PFM_INCLUDE_DIRS OR NOT PFM_LIBRARIES)
    message(FATAL_ERROR "libpfm4 not found. Please install libpfm4-dev")
endif()

#####################################
# Common target defaults
#####################################
function(mars_apply_target_defaults target_name)
    target_include_directories(${target_name}
        PUBLIC
            $<BUILD_INTERFACE:${MARS_INCLUDE_DIR}>
            $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(${target_name}
        PUBLIC
            mars::options
            mars::sanitizers
        PRIVATE
            mars::warnings
    )
endfunction()

#####################################
# Compile options
#####################################
add_library(mars_options INTERFACE)
add_library(mars::options ALIAS mars_options)
target_compile_features(mars_options INTERFACE cxx_std_17)
target_compile_definitions(mars_options INTERFACE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

#####################################
# Warnings
#####################################
add_library(mars_warnings INTERFACE)
add_library(mars::warnings ALIAS mars_warnings)
target_compile_options(mars_warnings INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<BOOL:${MARS_ENABLE_WERROR}>>:/WX>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
    $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<BOOL:${MARS_ENABLE_WERROR}>>:-Werror>
)

#####################################
# Sanitizers
#####################################
add_library(mars_sanitizers INTERFACE)
add_library(mars::sanitizers ALIAS mars_sanitizers)
target_compile_options(mars_sanitizers INTERFACE
    $<$<AND:$<BOOL:${MARS_ENABLE_SANITIZERS}>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-fsanitize=address,undefined -fno-omit-frame-pointer>
)
target_link_options(mars_sanitizers INTERFACE
    $<$<AND:$<BOOL:${MARS_ENABLE_SANITIZERS}>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-fsanitize=address,undefined>
)

#####################################
# Infra: logging
#####################################
add_library(mars_infra_logging INTERFACE)
add_library(mars::infra_logging ALIAS mars_infra_logging)
target_include_directories(mars_infra_logging
    INTERFACE
        $<BUILD_INTERFACE:${MARS_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(mars_infra_logging
    INTERFACE
        mars::options
        mars::sanitizers
        mars::warnings
        spdlog::spdlog
)

#####################################
# Platform
#####################################
add_library(mars_platform STATIC)
add_library(mars::platform ALIAS mars_platform)

if(UNIX AND NOT APPLE)
    target_sources(mars_platform PRIVATE 
        src/platform/linux/os_linux.cpp
        src/platform/linux/pmc_linux.cpp
        src/platform/linux/events_discovery_linux.cpp
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

#####################################
# Core
#####################################
add_library(mars_core INTERFACE)
add_library(mars::core ALIAS mars_core)
target_include_directories(mars_core
    INTERFACE
        $<BUILD_INTERFACE:${MARS_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(mars_core
    INTERFACE
        mars::options
        mars::sanitizers
        mars::warnings
        mars::infra_logging
)

#####################################
# Cache
#####################################
add_library(mars_cache INTERFACE)
add_library(mars::cache ALIAS mars_cache)
target_include_directories(mars_cache
    INTERFACE
        $<BUILD_INTERFACE:${MARS_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(mars_cache
    INTERFACE
        mars::options
        mars::sanitizers
        mars::warnings
        mars::core
        mars::platform
        mars::infra_logging
)

#####################################
# ROB (Reorder Buffer Measurement)
#####################################
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

#####################################
# App
#####################################
add_library(mars_app STATIC
    src/app/application.cpp
    src/app/config_loader.cpp
)
add_library(mars::app ALIAS mars_app)

mars_apply_target_defaults(mars_app)

target_link_libraries(mars_app
    PUBLIC
        mars::cache
        mars::core
        mars::infra_logging
        mars::rob
        CLI11::CLI11
    PRIVATE
        yaml-cpp
        asmjit::asmjit
)

#####################################
# CLI applications
#####################################
if(MARS_BUILD_APPS)
    add_executable(mars
        src/cli/main.cpp
    )
    target_link_libraries(mars
        PRIVATE
            mars::app
            mars::warnings
    )
endif()

find_program(SETCAP_PATH setcap)
if(SETCAP_PATH)
    add_custom_command(TARGET mars POST_BUILD
        COMMAND sudo ${SETCAP_PATH} cap_perfmon=eip $<TARGET_FILE:mars>
        COMMENT "Setting cap_perfmon capability for PMC access"
    )
endif()

#####################################
# Tests
#####################################
if(MARS_BUILD_TESTS)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
        enable_testing()
        add_subdirectory(tests)
    else()
        message(STATUS "MARS_BUILD_TESTS is ON, but tests/ is not present")
    endif()
endif()
