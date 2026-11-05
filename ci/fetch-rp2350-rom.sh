#!/bin/sh
# Downloads Raspberry Pi's RP2350-A2 boot ROM, which the RP2350 boards can run in place of
# picocalc-sim's own, and checks it is the published file.
#   ci/fetch-rp2350-rom.sh FILE
set -eu
out=$1
url=https://github.com/raspberrypi/pico-bootrom-rp2350/releases/download/A2/bootrom-combined.bin
sum=196c503fbb17b2ad21a1f496574786af698f9376af9a5f79e5e75293c9e12191
mkdir -p "$(dirname "$out")"
curl -sSfL -o "$out.part" "$url"
got=$(sha256sum "$out.part" | cut -d' ' -f1)
if [ "$got" != "$sum" ]; then
    echo "$url has SHA-256 $got, not $sum" >&2
    rm -f "$out.part"
    exit 1
fi
mv "$out.part" "$out"
echo "fetched the RP2350 boot ROM to $out"
