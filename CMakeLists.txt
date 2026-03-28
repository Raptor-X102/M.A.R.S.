cmake_minimum_required(VERSION 3.10)
project(SiliconProbe CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pthread")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ------------------------------------------------------------------
# Базовые директории с заголовками для всех целей
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/arch
    ${CMAKE_SOURCE_DIR}/src/os
    ${CMAKE_SOURCE_DIR}/src/logger
    ${CMAKE_SOURCE_DIR}/src/measurers
    ${CMAKE_SOURCE_DIR}/src/measurers/cache_measurer
    ${CMAKE_SOURCE_DIR}/src/cpu_info
)

# ------------------------------------------------------------------
# Logger - статическая библиотека
add_library(logger STATIC 
    src/logger/logger.cpp
)

target_include_directories(logger PUBLIC ${CMAKE_SOURCE_DIR}/src/logger)

# ------------------------------------------------------------------
# OS - статическая библиотека
add_library(os STATIC)

# Добавляем исходники в зависимости от платформы
if(UNIX AND NOT APPLE)
    target_sources(os PRIVATE src/os/os_linux.cpp)
elseif(WIN32)
    target_sources(os PRIVATE src/os/os_windows.cpp)
elseif(APPLE)
    target_sources(os PRIVATE src/os/os_macos.cpp)
else()
    message(FATAL_ERROR "Unsupported operating system")
endif()

target_include_directories(os PUBLIC ${CMAKE_SOURCE_DIR}/src/os)
target_link_libraries(os PRIVATE logger)

# Для Linux добавляем pthread
if(UNIX AND NOT APPLE)
    target_link_libraries(os PRIVATE pthread)
endif()

# ------------------------------------------------------------------
# Cache Profiler - статическая библиотека
add_library(cache_profiler STATIC
    src/measurers/cache_measurer/cache_measurer.cpp
    src/measurers/cache_measurer/cache_profiler_list.cpp
)

target_include_directories(cache_profiler PUBLIC 
    ${CMAKE_SOURCE_DIR}/src/measurers/cache_measurer
)

target_link_libraries(cache_profiler 
    PUBLIC
        os
        logger
)

# ------------------------------------------------------------------
# Measurers - интерфейсная библиотека
add_library(measurers INTERFACE)

target_include_directories(measurers INTERFACE 
    ${CMAKE_SOURCE_DIR}/src/measurers
)

target_link_libraries(measurers 
    INTERFACE
        cache_profiler
        os
        logger
)

# ------------------------------------------------------------------
# CPU Info - статическая библиотека
add_library(cpu_info STATIC
    src/cpu_info/cpu_info.cpp
)

target_include_directories(cpu_info PUBLIC 
    ${CMAKE_SOURCE_DIR}/src/cpu_info
)

target_link_libraries(cpu_info
    PUBLIC
        measurers
        logger
    PRIVATE
        cache_profiler
        os
)

# ------------------------------------------------------------------
# Исполняемый файл
add_executable(silicon_probe
    src/main.cpp
)

target_link_libraries(silicon_probe
    PRIVATE
        cpu_info
        measurers
        cache_profiler
        os
        logger
)

# ------------------------------------------------------------------
# Тесты (опционально)
# enable_testing()
# add_subdirectory(tests)
