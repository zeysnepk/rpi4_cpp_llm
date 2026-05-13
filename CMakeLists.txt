cmake_minimum_required(VERSION 3.14)
project(rpi4_dashboard CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)

add_compile_options(-O3 -mcpu=cortex-a72 -mtune=cortex-a72 -Wall)

find_package(Threads REQUIRED)

add_executable(dashboard src/dashboard.cpp)

target_include_directories(dashboard PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(dashboard PRIVATE Threads::Threads)