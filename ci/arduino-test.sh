#!/bin/sh
# Runs tests/arduino's checks of what arduino-pico firmware relies on, I2C bit-banged on the keyboard
# controller's pins and the PIO cycle counter, and fails unless it prints ARDUINO PASS over UART0.
#   ci/arduino-test.sh PICOCALC_SIM UF2
set -u
picocalc_sim=$1
uf2=$2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

PICOCALC_SIM_STOP_AT_MS=2000 timeout 120 "$picocalc_sim" --headless --unthrottled --console \
    --flash none --no-card --board pico2 "$uf2" >"$work/out.log" 2>"$work/err.log"
rc=$?
summary=$(grep '^ARDUINO ' "$work/out.log")
if [ $rc -ne 0 ] || [ "${summary#ARDUINO PASS}" = "$summary" ]; then
    echo "FAIL $(basename "$uf2"): ${summary:-no result (exit $rc)}"
    sed 's/^/    /' "$work/out.log" "$work/err.log"
    exit 1
fi
echo "ok   $(basename "$uf2") (${summary#ARDUINO PASS: })"
