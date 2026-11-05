#!/bin/sh
# Runs ftpserver on each W board behind the emulated WiFi and checks that an FTP client on the host
# can log in, list, download and upload through a port forward.
#   ci/wifi-test.sh PICOCALC_SIM MKCARD FTPSERVER.uf2...
# PICOCALC_SIM_ROM, if set, is the RP2040 boot ROM for the Pico W image, and PICOCALC_SIM_RP2350_ROM
# Raspberry Pi's RP2350 ROM to run in place of picocalc-sim's own.
set -u
picocalc_sim=$1
mkcard=$2
shift 2
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/card"
cat >"$work/card/ftpsrv.ini" <<'EOF'
[ftp]
enabled = on
port = 21
passive = 50000-50015
user = picocalc
password = sim
[wifi]
ssid = PicoCalc-CI
password = ci-password
country = US
EOF
head -c 1048576 /dev/urandom >"$work/card/big.bin"

port=21021
failed=0
for uf2; do
    name=$(basename "$uf2")
    "$mkcard" "$work/sd.img" "$work/card" >/dev/null || exit 1
    "$picocalc_sim" --headless --flash none --card "$work/sd.img" ${PICOCALC_SIM_ROM:+--rom "$PICOCALC_SIM_ROM"} \
        ${PICOCALC_SIM_RP2350_ROM:+--rp2350-rom "$PICOCALC_SIM_RP2350_ROM"} --wifi-ap PicoCalc-CI:ci-password \
        --forward "$port:21" -t 200 "$uf2" >"$work/log" 2>&1 &
    pid=$!
    if timeout 190 python3 -I "$here/ftp-client.py" 127.0.0.1 "$port" "$work/card/big.bin" >"$work/out" 2>&1; then
        echo "ok   $name ($(sed -n 's/^RETR big.bin: //p' "$work/out"))"
    else
        echo "FAIL $name"
        sed 's/^/    /' "$work/out" "$work/log"
        failed=1
    fi
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    port=$((port + 1))
done
exit $failed
