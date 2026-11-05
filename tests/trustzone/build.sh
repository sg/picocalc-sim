#!/bin/sh
# Rebuilds trustzone.uf2, which is kept prebuilt so CI needs no Arm compiler.
#   tests/trustzone/build.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
flags="-mcpu=cortex-m33 -mthumb -mfloat-abi=soft -O1 -ffreestanding -fno-builtin -nostdlib -Wall -Wextra"
arm-none-eabi-gcc $flags -c "$here/start.S" -o "$work/start.o"
arm-none-eabi-gcc $flags -c "$here/trustzone.c" -o "$work/trustzone.o"
arm-none-eabi-gcc $flags -T "$here/link.ld" "$work/start.o" "$work/trustzone.o" -o "$work/trustzone.elf" -lgcc
arm-none-eabi-objcopy -O binary "$work/trustzone.elf" "$work/trustzone.bin"
python3 "$here/mkuf2.py" "$work/trustzone.bin" "$here/trustzone.uf2"
echo "wrote $here/trustzone.uf2"
