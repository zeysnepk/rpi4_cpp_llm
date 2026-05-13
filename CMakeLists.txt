cmake_minimum_required(VERSION 3.14)
project(my_llm CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)

add_compile_options(-O3 -mcpu=cortex-a72 -mtune=cortex-a72)

set(LLAMA_DIR   "$ENV{HOME}/llama.cpp")
set(LLAMA_BUILD "${LLAMA_DIR}/build")
set(LLAMA_LIBS  "${LLAMA_BUILD}/bin")   # <-- yeni llama.cpp'de tum .so'lar burada

# Header yollari
include_directories(
    ${LLAMA_DIR}/include
    ${LLAMA_DIR}/ggml/include
)

# Tum kutuphaneler tek klasorde
find_library(LLAMA_LIB      NAMES llama       PATHS ${LLAMA_LIBS} NO_DEFAULT_PATH REQUIRED)
find_library(GGML_LIB       NAMES ggml        PATHS ${LLAMA_LIBS} NO_DEFAULT_PATH REQUIRED)
find_library(GGML_BASE_LIB  NAMES ggml-base   PATHS ${LLAMA_LIBS} NO_DEFAULT_PATH REQUIRED)
find_library(GGML_CPU_LIB   NAMES ggml-cpu    PATHS ${LLAMA_LIBS} NO_DEFAULT_PATH REQUIRED)

find_package(Threads REQUIRED)
find_package(OpenMP  REQUIRED)

add_executable(my_llm main.cpp)

target_link_libraries(my_llm PRIVATE
    ${LLAMA_LIB}
    ${GGML_CPU_LIB}
    ${GGML_LIB}
    ${GGML_BASE_LIB}
    Threads::Threads
    OpenMP::OpenMP_CXX
    dl
    m
)

# Runtime'da .so'lari bulabilsin
set_target_properties(my_llm PROPERTIES
    BUILD_RPATH   "${LLAMA_LIBS}"
    INSTALL_RPATH "${LLAMA_LIBS}"
)