#!/bin/bash
set -e  # hata verince script durur

# ============================================================
# Platform-aware llama-server launcher
# ============================================================

UNAME=$(uname -s)

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)" # proje kok dizini

if [[ "$UNAME" == "Darwin" ]]; then
    LLAMA_BIN="$PROJECT_DIR/models/llama.cpp/build/bin/llama-server"
    MODEL_DIR="$PROJECT_DIR/models"
    THREADS=4
    PLATFORM="Mac"
elif [[ "$UNAME" == "Linux" ]]; then
    LLAMA_BIN="$HOME/llama.cpp/build/bin/llama-server"
    MODEL_DIR="$HOME/models"
    THREADS=4
    PLATFORM="RPi/Linux"
else
    echo "Unsupported platform: $UNAME"
    exit 1
fi

# ============================================================
# MODEL
# ============================================================
MODEL_FILE="Qwen2.5-0.5B-Instruct-Q4_K_M.gguf"

MODEL="$MODEL_DIR/$MODEL_FILE"

if [[ ! -f "$LLAMA_BIN" ]]; then
    echo "ERROR: llama-server not found: $LLAMA_BIN"
    exit 1
fi
if [[ ! -f "$MODEL" ]]; then
    echo "ERROR: Model not found: $MODEL"
    echo "Available models:"
    ls -lh "$MODEL_DIR"/*.gguf 2>/dev/null || echo "  (no .gguf files)"
    exit 1
fi

echo "═══════════════════════════════════════════════"
echo " Platform : $PLATFORM"
echo " Binary   : $(basename $LLAMA_BIN)"
echo " Model    : $MODEL_FILE"
echo " Threads  : $THREADS"
echo "═══════════════════════════════════════════════"

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