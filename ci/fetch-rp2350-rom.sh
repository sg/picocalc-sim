#!/bin/sh
# Downloads Raspberry Pi's RP2350 boot ROM for stepping A2 or A4, which the RP2350 boards can run
# in place of picocalc-sim's own, and checks it is the published file.
#   ci/fetch-rp2350-rom.sh A2|A4 FILE
set -eu
rev=$1
out=$2
case $rev in
A2) sum=196c503fbb17b2ad21a1f496574786af698f9376af9a5f79e5e75293c9e12191 ;;
A4) sum=c5f9c48c6539f043c423344a53ceaea3774dd1e4c337af49da741a5d259c84bc ;;
*)
    echo "no known RP2350 boot ROM $rev" >&2
    exit 1
    ;;
esac
url=https://github.com/raspberrypi/pico-bootrom-rp2350/releases/download/$rev/bootrom-combined.bin
mkdir -p "$(dirname "$out")"
curl -sSfL -o "$out.part" "$url"
got=$(sha256sum "$out.part" | cut -d' ' -f1)
if [ "$got" != "$sum" ]; then
    echo "$url has SHA-256 $got, not $sum" >&2
    rm -f "$out.part"
    exit 1
fi
mv "$out.part" "$out"
echo "fetched the RP2350-$rev boot ROM to $out"
