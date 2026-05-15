#!/bin/bash

# Modeller:
# qwen2.5-3b-instruct-q4_k_m.gguf
# gemma-2-2b-it-Q4_K_M.gguf
# tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf
# Phi-3-mini-4k-instruct-q4.gguf
# Llama-3.2-3B-Instruct-Q4_K_M.gguf

~/llama.cpp/build/bin/llama-server \
  -m ~/models/qwen2.5-3b-instruct-q4_k_m.gguf \
  --host 127.0.0.1 \
  --port 8080 \
  -t 3 \
  -c 4096 \
  -b 256 \
  --jinja \
  --mlock \
  --cache-type-k q8_0 \
  --cache-type-v q8_0 \
  -ngl 0