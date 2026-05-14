cmake_minimum_required(VERSION 3.14)
project(rpi4_dashboard CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)

add_compile_options(-O3 -mcpu=cortex-a72 -mtune=cortex-a72 -Wall)

find_package(Threads REQUIRED)

# libgpiod (pkg-config araciligiyla)
find_path(GPIOD_INCLUDE gpiod.h REQUIRED)
find_library(GPIOD_LIB NAMES gpiod REQUIRED)

file(GLOB SENSOR_SOURCES src/sensors/*.cpp)

add_executable(dashboard
    src/dashboard.cpp
    src/sensor_manager.cpp
    src/gpio_power.cpp           # <-- YENI
    ${SENSOR_SOURCES}
)

target_include_directories(dashboard PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/sensors
    ${GPIOD_INCLUDE}
)

target_link_libraries(dashboard PRIVATE
    Threads::Threads
    ${GPIOD_LIB}                  # <-- YENI
)