#!/bin/bash
# RPi4 ilk kurulum scripti
# Kullanım: bash setup_rpi.sh
# Bağımlılıkları kurar, projeyi derler, modeli hazır hale getirir.
set -e # hata verince script durur

echo "=== Muhtas RPi4 Kurulum ==="

# 1. Sistem bağımlılıkları
echo "[1/5] Bağımlılıklar kuruluyor..."
sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends \
    build-essential cmake git libgpiod-dev i2c-tools

# 2. I2C etkin mi kontrol et
if ! ls /dev/i2c-* &>/dev/null; then
    echo "UYARI: I2C arayüzü bulunamadı."
    echo "       sudo raspi-config → Interface Options → I2C → Enable"
    echo "       Sonra 'sudo reboot' yapıp bu scripti tekrar çalıştır."
fi

# 3. Projeyi derle
echo "[2/5] Dashboard derleniyor..."
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -- -j4
echo "      Build OK -> ./build/dashboard"

# 4. llama.cpp kurulumu (yoksa)
LLAMA_DIR="$HOME/llama.cpp"
if [ ! -f "$LLAMA_DIR/build/bin/llama-server" ]; then
    echo "[3/5] llama.cpp derleniyor (~10 dk)..."
    if [ ! -d "$LLAMA_DIR" ]; then
        git clone --depth 1 https://github.com/ggerganov/llama.cpp "$LLAMA_DIR"
    fi
    cmake -B "$LLAMA_DIR/build" "$LLAMA_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DGGML_NATIVE=OFF \
        -DLLAMA_CURL=OFF
    cmake --build "$LLAMA_DIR/build" --config Release -j4 --target llama-server
    echo "      llama-server OK"
else
    echo "[3/5] llama.cpp zaten mevcut, atlandı."
fi

# 5. Model kontrolü
MODEL_DIR="$HOME/models"
mkdir -p "$MODEL_DIR"
echo "[4/5] Model kontrolü..."
if ls "$MODEL_DIR"/*.gguf &>/dev/null; then
    echo "      Modeller:"
    ls -lh "$MODEL_DIR"/*.gguf
else
    echo "UYARI: $MODEL_DIR içinde .gguf yok."
    echo "       Modeli Mac'ten kopyala:"
    echo "       scp models/qwen3-1.7b.Q4_K_M.gguf pi@<RPi-IP>:~/models/"
fi

# 6. Özet
echo ""
echo "[5/5] Hazır! Başlatmak için:"
echo "      Terminal 1: ./scripts/start_server.sh"
echo "      Terminal 2: ./build/dashboard"
echo ""
echo "Sensör testi: i2cdetect -y 1"
echo "  0x76/0x77 = BME280, 0x68 = MPU6500, 0x0D = QMC5883L"
