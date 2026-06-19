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
# MODEL
# ============================================================
MODEL_FILE="Qwen3-1.7B-Q4_K_M.gguf"   # stock model — full English pipeline

# Alternatifler:
# MODEL_FILE="qwen3-1.7b.Q4_K_M_v5.gguf"   # TR fine-tune (eski)
# MODEL_FILE="qwen3-1.7b.Q4_K_M_v2.gguf"
# MODEL_FILE="Qwen2.5-0.5B-Instruct-Q4_K_M.gguf"

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

# Qwen3: --jinja (chat template render) + thinking mode disabled
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
    -ngl 0 \
    --jinja \
    --chat-template-kwargs '{"enable_thinking": false}'