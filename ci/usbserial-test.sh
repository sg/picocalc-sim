#!/bin/sh
# Runs tests/usbserial's USB serial check with this shell's stdin and stdout as the terminal on the
# port (--usb-console), and fails unless the firmware got the line sent to it intact and every line
# it sent back arrived.
#   ci/usbserial-test.sh PICOCALC_SIM UF2
# PICOCALC_SIM_ROM, if set, is the RP2040 boot ROM for the Pico build, and PICOCALC_SIM_RP2350_ROM
# is Raspberry Pi's RP2350 ROM to boot the Pico 2 build on instead of picocalc-sim's own.
set -u
picocalc_sim=$1
uf2=$2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

line=$(awk 'BEGIN { for (i = 0; i < 300; i++) printf "%c", 33 + (i * 7) % 94 }')
want=$(printf '%s' "$line" | awk 'BEGIN { for (i = 32; i < 127; i++) ord[sprintf("%c", i)] = i }
    { for (i = 1; i <= length($0); i++) h = (h * 31 + ord[substr($0, i, 1)]) % 4294967296 }
    END { printf "USBSER GOT %d %08x\n", length($0), h }')
awk 'BEGIN { for (i = 0; i < 100; i++) { s = ""; for (j = 0; j < 48; j++) s = s sprintf("%c", 65 + (i + j) % 26)
    printf "USBSER LINE %03d %s\n", i, s } }' >"$work/want.txt"

printf '%s\r' "$line" | PICOCALC_SIM_STOP_AT_MS=4000 timeout 120 "$picocalc_sim" --headless --unthrottled --trace \
    --flash none --no-card --usb-console ${PICOCALC_SIM_ROM:+--rom "$PICOCALC_SIM_ROM"} \
    ${PICOCALC_SIM_RP2350_ROM:+--rp2350-rom "$PICOCALC_SIM_RP2350_ROM"} "$uf2" >"$work/out.log" 2>"$work/err.log"
rc=$?
tr -d '\r' <"$work/out.log" >"$work/out.txt"
got=$(grep '^USBSER GOT' "$work/out.txt")
grep '^USBSER LINE' "$work/out.txt" >"$work/lines.txt"
if [ $rc -ne 0 ] || [ "$got" != "$want" ] || ! cmp -s "$work/want.txt" "$work/lines.txt" ||
    ! grep -q '^USBSER DONE' "$work/out.txt"; then
    echo "FAIL $(basename "$uf2"): ${got:-nothing received} (want $want, exit $rc)"
    diff "$work/want.txt" "$work/lines.txt" | head -n 10 | sed 's/^/    /'
    sed 's/^/    /' "$work/out.txt" "$work/err.log" | head -n 40
    exit 1
fi
echo "ok   $(basename "$uf2") (${got#USBSER GOT }, $(wc -l <"$work/lines.txt") lines back)"
