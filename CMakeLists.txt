cmake_minimum_required(VERSION 3.14)
project(rpi4_dashboard CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-O3 -Wall)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "(aarch64|armv7l)")
    add_compile_options(-mcpu=cortex-a72 -mtune=cortex-a72)
    message(STATUS "ARM detected: Cortex-A72 optimizations enabled")
endif()

find_package(Threads REQUIRED)

add_executable(dashboard
    src/dashboard.cpp
    src/sensor_manager.cpp
    src/tool_dispatcher.cpp
    src/intent_router.cpp
    src/analyzer.cpp
    src/translator.cpp
    src/gpio_power.cpp
    src/sensors/i2c_bus.cpp
    src/sensors/bme280.cpp
    src/sensors/mpu6050.cpp
    src/sensors/qmc5883l.cpp
    src/sensors/sim_bme280.cpp
    src/sensors/sim_mpu6050.cpp
    src/sensors/sim_qmc5883l.cpp
)

target_include_directories(dashboard PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/sensors
)

target_link_libraries(dashboard PRIVATE Threads::Threads)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_library(GPIOD_LIB NAMES gpiod)
    if(GPIOD_LIB)
        target_link_libraries(dashboard PRIVATE ${GPIOD_LIB})
        message(STATUS "libgpiod bulundu: ${GPIOD_LIB}")
    else()
        message(WARNING "libgpiod bulunamadi - GPIO calismayacak")
    endif()
endif()