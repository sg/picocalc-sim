#!/bin/sh
# Runs tests/trustzone's self-checking test of the RP2350's security extension and fails unless
# every check it prints over UART0 passes.
#   ci/trustzone-test.sh PICOCALC_SIM UF2
# PICOCALC_SIM_RP2350_ROM, if set, is Raspberry Pi's RP2350 ROM to boot it on instead of
# picocalc-sim's own.
set -u
picocalc_sim=$1
uf2=$2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# The ROM takes about a second to launch a RAM image after its boot checks.
PICOCALC_SIM_STOP_AT_MS=3000 timeout 120 "$picocalc_sim" --headless --unthrottled --console --trace --flash none \
    --no-card ${PICOCALC_SIM_RP2350_ROM:+--rp2350-rom "$PICOCALC_SIM_RP2350_ROM"} "$uf2" >"$work/out.log" 2>"$work/err.log"
rc=$?
summary=$(grep '^TZ ' "$work/out.log")
if [ $rc -ne 0 ] || [ "${summary#TZ PASS}" = "$summary" ] || grep -q '^FAIL\|UNEXPECTED' "$work/out.log"; then
    echo "FAIL $(basename "$uf2"): ${summary:-no result (exit $rc)}"
    sed 's/^/    /' "$work/out.log" "$work/err.log"
    exit 1
fi
echo "ok   $(basename "$uf2") (${summary#TZ PASS: })"
