#!/bin/sh
# Boots a board with erased flash so its boot ROM presents its drive, copies a UF2 onto the drive
# over the emulated cable, and fails unless the ROM writes it, restarts into it, and the firmware
# then enumerates with the expected vendor and product IDs.
#   ci/usbdrive-test.sh PICOCALC_SIM BOARD UF2 VID:PID
# PICOCALC_SIM_ROM, if set, is the RP2040 boot ROM for the Pico and Pico W, and
# PICOCALC_SIM_RP2350_ROM the RP2350 boot ROM to run in place of Raspberry Pi's A4: A2 or a file,
# since picocalc-sim's stand-in has no USB boot.
set -u
picocalc_sim=$1
board=$2
uf2=$3
id=$4
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

printf 'usb copy %s\nwait 120000\n' "$uf2" >"$work/copy.pcs"
PICOCALC_SIM_STOP_AT_MS=6000 timeout 120 "$picocalc_sim" --headless --unthrottled --trace --flash none --erase-flash \
    --no-card --usb -b "$board" -s "$work/copy.pcs" ${PICOCALC_SIM_ROM:+--rom "$PICOCALC_SIM_ROM"} \
    ${PICOCALC_SIM_RP2350_ROM:+--rp2350-rom "$PICOCALC_SIM_RP2350_ROM"} >"$work/out.log" 2>&1
rc=$?
tr -d '\r' <"$work/out.log" >"$work/log.txt"
drive=$(sed -n 's/.*usb: drive \(".*"\) .*/\1/p' "$work/log.txt" | head -n 1)
copied=$(grep -c 'usb: copied ' "$work/log.txt")
# The firmware's own device, enumerated after the copy.
after=$(sed -n '/usb: copied /,$p' "$work/log.txt" | sed -n 's/.*usb: \([0-9a-f]*:[0-9a-f]*\) .*/\1/p' | head -n 1)
if [ $rc -ne 0 ] || [ -z "$drive" ] || [ "$copied" -ne 1 ] || [ "$after" != "$id" ]; then
    echo "FAIL $board $(basename "$uf2"): drive ${drive:-not mounted}, ${after:-nothing} enumerated after the copy (exit $rc)"
    sed 's/^/    /' "$work/log.txt"
    exit 1
fi
echo "ok   $board $(basename "$uf2") (drive $drive, then $after)"
