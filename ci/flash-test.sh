#!/bin/sh
# Runs tests/flash's erase, program and reprogram of flash through the boot ROM's routines, and fails
# unless it prints FLASH PASS over UART0 and writing flash left the ROM's code decoded.
#   ci/flash-test.sh PICOCALC_SIM UF2
set -u
picocalc_sim=$1
uf2=$2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# The test decodes about 1,500 blocks; decoding the ROM's routines again after every page or byte
# programmed took about 475,000.
max_blocks=20000

PICOCALC_SIM_STOP_AT_MS=3000 timeout 120 "$picocalc_sim" --headless --unthrottled --console --trace \
    --flash none --no-card --board pico2 "$uf2" >"$work/out.log" 2>"$work/err.log"
rc=$?
summary=$(grep '^FLASH ' "$work/out.log")
blocks=$(tr -d '\r' <"$work/err.log" | sed -n 's/.*code: \([0-9]*\) blocks decoded.*/\1/p' | tail -n 1)
if [ $rc -ne 0 ] || [ "${summary#FLASH PASS}" = "$summary" ]; then
    echo "FAIL $(basename "$uf2"): ${summary:-no result (exit $rc)}"
elif [ -z "$blocks" ]; then
    echo "FAIL $(basename "$uf2"): the trace report gave no count of blocks decoded"
elif [ "$blocks" -gt $max_blocks ]; then
    echo "FAIL $(basename "$uf2"): $blocks blocks decoded, more than $max_blocks"
else
    echo "ok   $(basename "$uf2") (${summary#FLASH PASS: }, $blocks blocks decoded)"
    exit 0
fi
sed 's/^/    /' "$work/out.log" "$work/err.log"
exit 1
