#!/bin/bash
~/llama.cpp/build/bin/llama-server \
  -m ~/models/qwen2.5-3b-instruct-q4_k_m.gguf \
  --host 127.0.0.1 \
  --port 8080 \
  -t 4 \
  -c 4096 \
  -b 256 \
  --mlock \
  --cache-type-k q8_0 \
  --cache-type-v q8_0 \
  -ngl 0