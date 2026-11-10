#!/bin/sh
# Rebuilds flash-pico2.uf2, which is kept prebuilt so CI needs no Pico SDK build. Needs
# PICO_SDK_PATH and the SDK's toolchain.
#   tests/flash/build.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cmake -S "$here" -B "$work/pico2" -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=MinSizeRel >"$work/pico2.log" 2>&1 ||
    { cat "$work/pico2.log"; exit 1; }
cmake --build "$work/pico2" -j"$(nproc)" >>"$work/pico2.log" 2>&1 || { cat "$work/pico2.log"; exit 1; }
cp "$work/pico2/flash.uf2" "$here/flash-pico2.uf2"
echo "wrote $here/flash-pico2.uf2"
