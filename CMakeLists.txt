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
option(MARS_SETCAP_AFTER_BUILD "Run setcap on the CLI binary after build" OFF)

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
if(TARGET yaml-cpp::yaml-cpp)
    set(MARS_YAMLCPP_TARGET yaml-cpp::yaml-cpp)
else()
    set(MARS_YAMLCPP_TARGET yaml-cpp)
endif()

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
# Tests
#####################################
if(MARS_BUILD_TESTS)
    find_package(GTest CONFIG QUIET)
    if(NOT GTest_FOUND)
        find_package(GTest REQUIRED)
    endif()

    include(GoogleTest)

    function(mars_add_test target_name)
        add_executable(${target_name} ${ARGN})
        target_link_libraries(${target_name}
            PRIVATE
                GTest::gtest_main
                mars::options
                mars::sanitizers
                mars::warnings
        )
        gtest_discover_tests(${target_name})
    endfunction()

    enable_testing()
endif()

add_subdirectory(include/infra)
add_subdirectory(include/platform)
add_subdirectory(include/measurement)
add_subdirectory(include/core)
add_subdirectory(src/app)

if(MARS_BUILD_TESTS)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/core/CMakeLists.txt)
        add_subdirectory(src/core)
    endif()
    add_subdirectory(src/measurement)
endif()

if(MARS_BUILD_APPS)
    add_subdirectory(src/cli)
endif()
