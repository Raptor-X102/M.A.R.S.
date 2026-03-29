cmake_minimum_required(VERSION 3.20)

project(SiliconProbe
  VERSION 0.1.0
  LANGUAGES CXX
)

option(SP_BUILD_APPS        "Build command line applications" ON)
option(SP_BUILD_TESTS       "Build tests" OFF)
option(SP_ENABLE_SANITIZERS "Enable ASan/UBSan (non-MSVC)" OFF)
option(SP_ENABLE_WERROR     "Treat warnings as errors" OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(SP_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

find_package(CLI11 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(Threads REQUIRED)

function(sp_apply_target_defaults target_name)
  target_include_directories(${target_name}
    PUBLIC
      $<BUILD_INTERFACE:${SP_INCLUDE_DIR}>
      $<INSTALL_INTERFACE:include>
  )

  target_link_libraries(${target_name}
    PUBLIC
      sp::options
      sp::sanitizers
    PRIVATE
      sp::warnings
  )
endfunction()

add_library(sp_options INTERFACE)
add_library(sp::options ALIAS sp_options)
target_compile_features(sp_options INTERFACE cxx_std_17)
target_compile_definitions(sp_options INTERFACE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

add_library(sp_warnings INTERFACE)
add_library(sp::warnings ALIAS sp_warnings)
target_compile_options(sp_warnings INTERFACE
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
  $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<BOOL:${SP_ENABLE_WERROR}>>:/WX>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
  $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<BOOL:${SP_ENABLE_WERROR}>>:-Werror>
)

add_library(sp_sanitizers INTERFACE)
add_library(sp::sanitizers ALIAS sp_sanitizers)
target_compile_options(sp_sanitizers INTERFACE
  $<$<AND:$<BOOL:${SP_ENABLE_SANITIZERS}>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-fsanitize=address,undefined -fno-omit-frame-pointer>
)
target_link_options(sp_sanitizers INTERFACE
  $<$<AND:$<BOOL:${SP_ENABLE_SANITIZERS}>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-fsanitize=address,undefined>
)

add_library(sp_infra_logging STATIC
  src/infra/logging.cpp
)
add_library(sp::infra_logging ALIAS sp_infra_logging)
sp_apply_target_defaults(sp_infra_logging)
target_link_libraries(sp_infra_logging PUBLIC spdlog::spdlog)

add_library(sp_platform STATIC)
add_library(sp::platform ALIAS sp_platform)
if(UNIX AND NOT APPLE)
  target_sources(sp_platform PRIVATE src/platform/linux/os_linux.cpp)
else()
  message(FATAL_ERROR "Unsupported platform: SiliconProbe currently supports Linux only")
endif()
sp_apply_target_defaults(sp_platform)
target_link_libraries(sp_platform
  PUBLIC
    sp::infra_logging
    Threads::Threads
)

add_library(sp_core STATIC
  src/core/measurer_registry.cpp
  src/core/probe_service.cpp
  src/core/summary_printer.cpp
)
add_library(sp::core ALIAS sp_core)
sp_apply_target_defaults(sp_core)
target_link_libraries(sp_core PUBLIC sp::infra_logging)

add_library(sp_cache STATIC
  src/cache/boundary_analyzer.cpp
  src/cache/cache_measurer.cpp
  src/cache/cache_profiler_list.cpp
)
add_library(sp::cache ALIAS sp_cache)
sp_apply_target_defaults(sp_cache)
target_link_libraries(sp_cache
  PUBLIC
    sp::core
    sp::platform
    sp::infra_logging
)

add_library(sp_app STATIC
  src/app/application.cpp
  src/app/config_loader.cpp
)
add_library(sp::app ALIAS sp_app)
sp_apply_target_defaults(sp_app)
target_link_libraries(sp_app
  PUBLIC
    sp::cache
    sp::core
    sp::infra_logging
    CLI11::CLI11
  PRIVATE
    yaml-cpp
)

if(SP_BUILD_APPS)
  add_executable(silicon_probe
    src/cli/main.cpp
  )
  target_link_libraries(silicon_probe
    PRIVATE
      sp::app
      sp::warnings
  )
endif()

if(SP_BUILD_TESTS)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    enable_testing()
    add_subdirectory(tests)
  else()
    message(STATUS "SP_BUILD_TESTS is ON, but tests/ is not present")
  endif()
endif()
