#!/bin/sh
# Rebuilds jit-pico2.uf2, which is kept prebuilt so CI needs no Pico SDK build, after checking
# step.h's EXPECTED against this computer's own result. Needs PICO_SDK_PATH and the SDK's toolchain.
#   tests/jit/build.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
printf '#include <stdio.h>\n#include "step.h"\nint main(void) { printf("%%08x %%08x\\n", run(), EXPECTED); }\n' >"$work/host.c"
cc -O2 -I"$here" -o "$work/host" "$work/host.c"
set -- $("$work/host")
[ "$1" = "$2" ] || { echo "step.h's EXPECTED should be 0x$1"; exit 1; }
cmake -S "$here" -B "$work/pico2" -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=MinSizeRel >"$work/pico2.log" 2>&1 ||
    { cat "$work/pico2.log"; exit 1; }
cmake --build "$work/pico2" -j"$(nproc)" >>"$work/pico2.log" 2>&1 || { cat "$work/pico2.log"; exit 1; }
cp "$work/pico2/jit.uf2" "$here/jit-pico2.uf2"
echo "wrote $here/jit-pico2.uf2"
