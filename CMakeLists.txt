# cmake -B build      -> CMakeLists.txt'i okur, build/ içine Makefile üretir
# cmake --build build -> üretilen Makefile'ı çalıştırıp programı derler

cmake_minimum_required(VERSION 3.14)
project(rpi4_dashboard CXX) # CXX -> C++

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release) # release -> optimize ve hizli, debug -> hata ayiklama
endif()

add_compile_options(-O3 -Wall) # O3 -> optimize ve hzili, Wall -> tum uyarilari goster

if(CMAKE_SYSTEM_PROCESSOR MATCHES "(aarch64|armv7l)") # rpi4 ise
    add_compile_options(-mcpu=cortex-a72 -mtune=cortex-a72) # cortex-a72 -> rpi4 cpu'su
    message(STATUS "ARM detected: Cortex-A72 optimizations enabled")
endif()

find_package(Threads REQUIRED) # Threads -> pthread kütüphanesini zorunlu

# dashboard adinda, tum dosyalari derler
add_executable(dashboard
    src/dashboard.cpp
    src/sensor_manager.cpp
    src/llm/tool_dispatcher.cpp
    src/llm/intent_router.cpp
    src/llm/analyzer.cpp
    src/hardware/gpio_power.cpp
    src/sensors/i2c_bus.cpp
    src/sensors/bme280.cpp
    src/sensors/mpu6050.cpp
    src/sensors/qmc5883l.cpp
    src/sensors/sim_bme280.cpp
    src/sensors/sim_mpu6050.cpp
    src/sensors/sim_qmc5883l.cpp
)

# include dizinleri, dashboard'un bu dizinlerdeki header dosyalarini bulmasi icin
target_include_directories(dashboard PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/inc
    ${CMAKE_SOURCE_DIR}/inc/sensors
)

# bulunan threads kütüphanesini dashboard programina baglar
target_link_libraries(dashboard PRIVATE Threads::Threads)

# Linux'ta libgpiod varsa onu da baglar, yoksa uyarir
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_library(GPIOD_LIB NAMES gpiod)
    if(GPIOD_LIB)
        target_link_libraries(dashboard PRIVATE ${GPIOD_LIB})
        message(STATUS "libgpiod found: ${GPIOD_LIB}")
    else()
        message(WARNING "libgpiod not found — GPIO will not work")
    endif()
endif()