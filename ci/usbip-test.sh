#!/bin/sh
# Exports the board's USB device over USB/IP and drives it from ci/usbip-client.py, a client that
# imports and enumerates it as Linux's would.
#   ci/usbip-test.sh PICOCALC_SIM BOARD drive UF2 VID:PID   copies UF2 onto the boot ROM's drive
#                                                           and wants the firmware then to enumerate
#   ci/usbip-test.sh PICOCALC_SIM BOARD serial UF2          talks to tests/usbserial's serial port
# PICOCALC_SIM_ROM, if set, is the RP2040 boot ROM for the Pico and Pico W, and
# PICOCALC_SIM_RP2350_ROM the RP2350 boot ROM to run in place of Raspberry Pi's A4, which for drive
# must be one of Raspberry Pi's: the stand-in has no USB boot.
set -u
picocalc_sim=$1
board=$2
mode=$3
uf2=$4
id=${5:-}
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
port=$((20000 + $$ % 20000))
if [ "$mode" = drive ]; then
    image="--erase-flash"
else
    image="$uf2"
fi
PICOCALC_SIM_STOP_AT_MS=60000 timeout 120 "$picocalc_sim" --headless --trace --flash none --no-card -b "$board" \
    --usbip "$port" ${PICOCALC_SIM_ROM:+--rom "$PICOCALC_SIM_ROM"} \
    ${PICOCALC_SIM_RP2350_ROM:+--rp2350-rom "$PICOCALC_SIM_RP2350_ROM"} $image >"$work/sim.log" 2>&1 &
sim=$!
trap 'kill $sim 2>/dev/null; rm -rf "$work"' EXIT
if [ "$mode" = drive ]; then
    timeout 90 python3 "$here/usbip-client.py" 127.0.0.1 "$port" drive "$uf2" >"$work/client.log" 2>&1
else
    timeout 90 python3 "$here/usbip-client.py" 127.0.0.1 "$port" serial >"$work/client.log" 2>&1
fi
rc=$?
after=
if [ "$mode" = drive ] && [ $rc -eq 0 ]; then
    # The boot ROM restarts into what was copied, and the firmware's own device then enumerates.
    for i in $(seq 1 40); do
        after=$(tr -d '\r' <"$work/sim.log" | sed -n '/usbip: the computer let go/,$p' |
            sed -n 's/.*usb: \([0-9a-f]*:[0-9a-f]*\) .*/\1/p' | grep -v '^2e8a:000[3f]$' | head -n 1)
        [ -n "$after" ] && break
        sleep 0.5
    done
fi
summary=$(tr '\n' ';' <"$work/client.log" | sed 's/;$//; s/;/; /g')
if [ $rc -ne 0 ] || { [ "$mode" = drive ] && [ "$after" != "$id" ]; }; then
    echo "FAIL $board $mode: ${summary:-no result} (exit $rc${after:+, then $after})"
    sed 's/^/    /' "$work/client.log" "$work/sim.log" | tail -n 40
    exit 1
fi
echo "ok   $board $mode ($summary${after:+; then $after})"
