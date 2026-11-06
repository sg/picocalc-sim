# Raspberry Pi's RP2350 boot ROMs

The on-chip boot ROMs of the RP2350's A2 stepping and of its A3 stepping as the A4 release, for
`--rp2350-rom` (see the main README's RP2350 boot ROM section):

```sh
build/picocalc-sim --rp2350-rom roms/rp2350/A4/bootrom-combined.bin ../pcsh/build-pico2/pcsh.uf2
```

They are `bootrom-combined.bin` as published at
<https://github.com/raspberrypi/pico-bootrom-rp2350/releases/tag/A2> and
<https://github.com/raspberrypi/pico-bootrom-rp2350/releases/tag/A4>, unchanged; `SHA256SUMS` has
their hashes. Raspberry Pi releases them under the BSD 3-clause licence in `LICENSE.TXT`, taken from
the same repository. Its exception for `bootrom/mufplib.S` and `bootrom/mufplib-double.S`, inherited
from the RP2040 ROM's repository, names files that neither release's source contains.

The RP2040's ROM is not here: its float library is licensed only for use on an RP2040, so the Pico
and Pico W boards need it from `--rom` (see the main README's RP2040 boards section).
