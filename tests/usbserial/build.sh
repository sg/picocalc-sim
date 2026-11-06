#!/bin/sh
# Rebuilds usbserial-pico2.uf2 and usbserial-pico.uf2, which are kept prebuilt so CI needs no Pico
# SDK build. Needs PICO_SDK_PATH and the SDK's toolchain.
#   tests/usbserial/build.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
for board in pico2 pico; do
    cmake -S "$here" -B "$work/$board" -DPICO_BOARD=$board -DCMAKE_BUILD_TYPE=MinSizeRel >"$work/$board.log" 2>&1 ||
        { cat "$work/$board.log"; exit 1; }
    cmake --build "$work/$board" -j"$(nproc)" >>"$work/$board.log" 2>&1 || { cat "$work/$board.log"; exit 1; }
    cp "$work/$board/usbserial.uf2" "$here/usbserial-$board.uf2"
    echo "wrote $here/usbserial-$board.uf2"
done
