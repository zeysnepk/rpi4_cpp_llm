#!/bin/bash
cd "$(dirname "$0")"

# RPi'de yuksek priority, Mac'te normal
if [[ "$(uname -s)" == "Linux" ]]; then
    PREFIX="sudo nice -n -10"
else
    PREFIX=""
fi

echo "Dashboard wrapper basladi (PID: $$)"

while true; do
    $PREFIX ./build/dashboard
    code=$?

    if [ $code -eq 42 ]; then
        echo ""
        echo "═══ Mode değişikliği — yeniden başlatılıyor ═══"
        sleep 1
        continue
    fi

    if [ $code -ne 0 ]; then
        echo "Dashboard hata ile çıktı (kod $code), wrapper durdu"
    else
        echo "Dashboard normal kapandı"
    fi
    exit $code
done