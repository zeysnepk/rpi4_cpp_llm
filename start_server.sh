#!/bin/bash
set -e

# ============================================================
# Platform-aware llama-server launcher
# ============================================================

UNAME=$(uname -s)

if [[ "$UNAME" == "Darwin" ]]; then
    LLAMA_BIN="/Volumes/ZeynepSSD/Projects/cpp_llm/models/llama.cpp/build/bin/llama-server"
    MODEL_DIR="/Volumes/ZeynepSSD/Projects/cpp_llm/models"
    THREADS=4
    PLATFORM="Mac"
elif [[ "$UNAME" == "Linux" ]]; then
    LLAMA_BIN="$HOME/llama.cpp/build/bin/llama-server"
    MODEL_DIR="$HOME/models"
    THREADS=3
    PLATFORM="RPi/Linux"
else
    echo "Desteklenmeyen platform: $UNAME"
    exit 1
fi

# ============================================================
# MODEL SECIMI
# IntentRouter mimarisinde LLM sadece yorumlama yapiyor.
# Kucuk model yeterli + cok daha hizli.
# ============================================================
MODEL_FILE="qwen2.5-1.5b-instruct-q4_k_m.gguf"

# Alternatifler:
# MODEL_FILE="qwen2.5-3b-instruct-q4_k_m.gguf"
# MODEL_FILE="Qwen2.5-0.5B-Instruct-Q4_K_M.gguf"        # en hizli, Turkce kalitesi azalir
# MODEL_FILE="Llama-3.2-3B-Instruct-Q4_K_M.gguf"
# MODEL_FILE="Hermes-3-Llama-3.2-3B-Q4_K_M.gguf"

MODEL="$MODEL_DIR/$MODEL_FILE"

if [[ ! -f "$LLAMA_BIN" ]]; then
    echo "HATA: llama-server bulunamadi: $LLAMA_BIN"
    exit 1
fi
if [[ ! -f "$MODEL" ]]; then
    echo "HATA: Model bulunamadi: $MODEL"
    echo "Mevcut modeller:"
    ls -lh "$MODEL_DIR"/*.gguf 2>/dev/null || echo "  (gguf yok)"
    exit 1
fi

echo "═══════════════════════════════════════════════"
echo " Platform : $PLATFORM"
echo " Bin      : $LLAMA_BIN"
echo " Model    : $MODEL_FILE"
echo " Threads  : $THREADS"
echo "═══════════════════════════════════════════════"

# NOT: --jinja artik gerekmiyor (tool calling LLM'de degil, kodla yapiliyor)
# Ama zarar vermez, biraktik
"$LLAMA_BIN" \
    -m "$MODEL" \
    --host 127.0.0.1 --port 8080 \
    -t $THREADS \
    -c 2048 \
    -b 128 \
    -np 1 \
    --mlock \
    --cache-type-k q8_0 \
    --cache-type-v q8_0 \
    -ngl 0