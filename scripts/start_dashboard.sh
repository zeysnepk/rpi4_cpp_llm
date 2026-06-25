#!/bin/bash
cd "$(dirname "$0")/.."

# RPi: pin dashboard to core 0 so it never steals llama-server's cores (1-3).
# (Previously ran at nice -10, which PREEMPTED llama-server and crippled token gen.)
if [[ "$(uname -s)" == "Linux" ]]; then
    PREFIX="sudo taskset -c 0"
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