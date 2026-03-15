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

# ------------------------------------------------------------------
# OS - статическая библиотека (теперь с реализацией)
add_library(os STATIC 
    src/os/os_linux.cpp
)

# ------------------------------------------------------------------
# Cache Profiler - статическая библиотека
add_library(cache_profiler STATIC
    src/measurers/cache_measurer/cache_measurer.cpp
    src/measurers/cache_measurer/cache_profiler_list.cpp
)

# Явно указываем зависимости для cache_profiler
target_link_libraries(cache_profiler 
    PUBLIC
        os
        logger
)

# ------------------------------------------------------------------
# Measurers - интерфейсная библиотека (объединяет все измерители)
add_library(measurers INTERFACE)

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
# Если нужны тесты - раскомментировать
# enable_testing()
# add_subdirectory(tests)
