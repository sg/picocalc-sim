#!/bin/sh
# Boots a board with erased flash, so its boot ROM presents its USB boot device, and fails unless
# the host on the emulated cable enumerates that device with the expected vendor and product IDs.
#   ci/usb-test.sh PICOCALC_SIM BOARD VID:PID
# PICOCALC_SIM_ROM, if set, is the RP2040 boot ROM for the Pico and Pico W, and
# PICOCALC_SIM_RP2350_ROM the RP2350 boot ROM to run in place of Raspberry Pi's A4: A2 or a file,
# since picocalc-sim's stand-in has no USB boot.
set -u
picocalc_sim=$1
board=$2
id=$3
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

PICOCALC_SIM_STOP_AT_MS=3000 timeout 120 "$picocalc_sim" --headless --unthrottled --trace --flash none --erase-flash \
    --no-card --usb -b "$board" ${PICOCALC_SIM_ROM:+--rom "$PICOCALC_SIM_ROM"} \
    ${PICOCALC_SIM_RP2350_ROM:+--rp2350-rom "$PICOCALC_SIM_RP2350_ROM"} >"$work/out.log" 2>&1
rc=$?
found=$(tr -d '\r' <"$work/out.log" | sed -n 's/.*usb: \([0-9a-f]*:[0-9a-f]*\) /\1 /p' | head -n 1)
if [ $rc -ne 0 ] || [ "${found%% *}" != "$id" ]; then
    echo "FAIL $board: ${found:-nothing enumerated (exit $rc)}"
    sed 's/^/    /' "$work/out.log"
    exit 1
fi
echo "ok   $board ($found)"
