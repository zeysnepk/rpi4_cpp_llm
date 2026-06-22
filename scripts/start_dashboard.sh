#!/bin/bash
cd "$(dirname "$0")/.."

# RPi: elevated priority; Mac: normal
if [[ "$(uname -s)" == "Linux" ]]; then
    PREFIX="sudo nice -n -10"
else
    PREFIX=""
fi

echo "Dashboard wrapper started (PID: $$)"

while true; do
    $PREFIX ./build/dashboard
    code=$?

    if [ $code -eq 42 ]; then
        echo ""
        echo "═══ Mode switch — restarting ═══"
        sleep 1
        continue
    fi

    if [ $code -ne 0 ]; then
        echo "Dashboard exited with error (code $code)"
    else
        echo "Dashboard stopped normally"
    fi
    exit $code
done