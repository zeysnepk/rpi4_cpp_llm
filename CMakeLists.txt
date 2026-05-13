cmake_minimum_required(VERSION 3.14)
project(my_llm CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)

add_compile_options(-O3 -mcpu=cortex-a72 -mtune=cortex-a72)

set(LLAMA_DIR "$ENV{HOME}/llama.cpp")

add_executable(my_llm main.cpp)

target_include_directories(my_llm PRIVATE
    ${LLAMA_DIR}/include
    ${LLAMA_DIR}/ggml/include
)

target_link_libraries(my_llm PRIVATE
    ${LLAMA_DIR}/build/src/libllama.so
    ${LLAMA_DIR}/build/ggml/src/libggml.so
    ${LLAMA_DIR}/build/ggml/src/libggml-base.so
    ${LLAMA_DIR}/build/ggml/src/libggml-cpu.so
)

set_target_properties(my_llm PROPERTIES
    BUILD_RPATH "${LLAMA_DIR}/build/src;${LLAMA_DIR}/build/ggml/src"
)