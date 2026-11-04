#!/bin/sh
# Downloads the RP2040-B2 boot ROM that the Pico and Pico W boards run, which can't be shipped
# with pcsim, and checks it is the published file.
#   ci/fetch-rp2040-rom.sh FILE
set -eu
out=$1
url=https://github.com/raspberrypi/pico-bootrom-rp2040/releases/download/b2/b2.elf
sum=3a3c4c617d83bc9b910a6fa7bfdffa11ce26ff89370271d861507be821cc49aa
mkdir -p "$(dirname "$out")"
curl -sSfL -o "$out.part" "$url"
got=$(sha256sum "$out.part" | cut -d' ' -f1)
if [ "$got" != "$sum" ]; then
    echo "$url has SHA-256 $got, not $sum" >&2
    rm -f "$out.part"
    exit 1
fi
mv "$out.part" "$out"
echo "fetched the RP2040 boot ROM to $out"
