#!/bin/sh
# Runs tests/arduino's checks of what arduino-pico firmware relies on, I2C bit-banged on the keyboard
# controller's pins and the PIO cycle counter, and fails unless it prints ARDUINO PASS over UART0
# and the counter's loops ran in bulk rather than an instruction at a time.
#   ci/arduino-test.sh PICOCALC_SIM UF2
set -u
picocalc_sim=$1
uf2=$2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# Stepped one at a time, the counter's loops would run about 25 million PIO instructions.
max_stepped=1000000

PICOCALC_SIM_STOP_AT_MS=2000 timeout 120 "$picocalc_sim" --headless --unthrottled --console --trace \
    --flash none --no-card --board pico2 "$uf2" >"$work/out.log" 2>"$work/err.log"
rc=$?
summary=$(grep '^ARDUINO ' "$work/out.log")
stepped=$(awk '/pio[0-9]: [0-9]+ instructions stepped/ { n += $(NF - 2); found = 1 } END { if (found) print n }' "$work/err.log")
if [ $rc -ne 0 ] || [ "${summary#ARDUINO PASS}" = "$summary" ]; then
    echo "FAIL $(basename "$uf2"): ${summary:-no result (exit $rc)}"
elif [ -z "$stepped" ]; then
    echo "FAIL $(basename "$uf2"): the trace report gave no count of PIO instructions stepped"
elif [ "$stepped" -gt $max_stepped ]; then
    echo "FAIL $(basename "$uf2"): $stepped PIO instructions stepped one at a time, more than $max_stepped"
else
    echo "ok   $(basename "$uf2") (${summary#ARDUINO PASS: }, $stepped PIO instructions stepped)"
    exit 0
fi
sed 's/^/    /' "$work/out.log" "$work/err.log"
exit 1
