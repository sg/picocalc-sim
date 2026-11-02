# picocalc_sim

A PicoCalc emulator for Linux and macOS (Windows to follow) that runs firmware as it ships: the
same UF2 file you would drop on the Pico's BOOTSEL drive or put in uf2loader's `pico2-apps`
folder. It emulates the RP2350 instruction by instruction, with the PicoCalc's LCD, keyboard
controller and SD card attached to its pins, so nothing has to be rebuilt for the simulator.

Boards: **Raspberry Pi Pico 2**, **Pico 2 W** and **Pimoroni Pico Plus 2 / 2 W** (16 MB flash,
8 MB PSRAM). The W boards boot and run, but their CYW43439 radio is not emulated yet, so firmware
reports that WiFi failed to start (see [Roadmap](#roadmap)).

## Building

You need CMake 3.16+ and a C compiler. Nothing from the Pico SDK or an ARM toolchain is needed.

```sh
cmake -S . -B build
cmake --build build -j
```

If SDL2 is not installed the build downloads SDL 2.30.9 (hash-pinned) and links it statically.
Installing the system package (`libsdl2-dev`, `brew install sdl2`) skips that; `-DPCSIM_SDL=OFF`
builds a headless-only binary. FatFs, used by the card-image tool, comes from a sibling
`ftpserver`, `picocalc_lua` or `picocalc-z80` checkout if there is one and is fetched otherwise.

The build produces `build/pcsim`, the emulator, and `build/tools/mkcard/pcsim-mkcard`, which makes
SD card images.

## Running firmware

```sh
build/pcsim ../pcsh/build-pico2/pcsh.uf2
```

A UF2 on the command line is written into flash as BOOTSEL's drive would write it, then the board
boots. Without one, the board boots whatever its flash already holds, like switching the PicoCalc
on. The board is taken from the image's binary info (`pico2`, `pico2_w`,
`pimoroni_pico_plus2_w_rp2350`); `--board` overrides it.

Each board's flash persists in `~/.local/share/pcsim/BOARD.flash` (`--flash FILE` to choose,
`--flash none` to discard it on exit, `--erase-flash` to start blank). The SD card is
`~/.local/share/pcsim/sd.img` if it exists, or `--card IMAGE`:

```sh
build/tools/mkcard/pcsim-mkcard ~/.local/share/pcsim/sd.img ../pcsh/sd_files
```

`pcsim-mkcard IMAGE [-s MB] [-l LABEL] [DIR...]` formats an MBR/FAT32 card (512 MB by default)
and copies each directory's contents to the root. Files the firmware writes persist in the image.

### uf2loader

pelrun's [uf2loader](https://github.com/pelrun/uf2loader) works as it does on a real PicoCalc:

```sh
mkdir -p card/pico2-apps
cp BOOT2350.uf2 card/BOOT2350.UF2
cp ../pcsh/build-pico2/pcsh.uf2 ../picocalc_lua/build-pico2/picolua.uf2 card/pico2-apps/
build/tools/mkcard/pcsim-mkcard ~/.local/share/pcsim/sd.img card
build/pcsim --board pico2 bootloader_pico2.uf2
```

With no app in flash the loader opens its menu; choosing a UF2 flashes it into the app partition
and boots it. Later runs (`build/pcsim` with no UF2) boot the app directly; hold F1 (or Up) and
press F12 to reset into the menu instead.

### Keys

The PC keyboard maps onto the PicoCalc's keys. Keys the PicoCalc lacks are sent as the Shift
chords its keyboard controller uses for them:

| PC key | Sent as |
| --- | --- |
| PgUp / PgDn | Shift+Up / Shift+Down |
| Home / End | Shift+Tab / Shift+Del |
| Insert | Shift+Enter |
| Pause | Shift+Esc (Brk) |
| F6-F10 | Shift+F1-F5 |
| **F11** | screenshot (emulator only) |
| **F12** | the Pico's reset; **Shift+F12** resets into BOOTSEL |

In BOOTSEL, drop a UF2 file on the window to flash it. Holding a key does not auto-repeat, as on
the real keyboard. Key mapping assumes a US layout. When the firmware powers the PicoCalc off,
any key powers it back on.

### Options

| Option | Effect |
| --- | --- |
| `-b`, `--board NAME` | `pico2`, `pico2_w`, `pimoroni_pico_plus2_w` or `pimoroni_pico_plus2` |
| `-f`, `--flash FILE` | flash image to use and keep |
| `-c`, `--card IMAGE` / `--no-card` | SD card image, or an empty slot |
| `--console` | print UART0 output, as a serial adapter on the PicoCalc's header would show |
| `--fast` | SPI, I2C and UART transfers complete instantly |
| `--unthrottled` | run as fast as the host allows instead of at device speed |
| `--battery PCT`, `--charging` | what the keyboard controller reports |
| `--scale N` | window scale (default 2) |
| `--trace` | log faults, unknown ROM calls and bus errors, and dump the cores on exit |

### Scripts and headless runs

`--headless --script FILE` drives the keyboard from a file and saves PNGs of the panel; it needs
no display, so it works in CI. `scripts/pcsh-smoke.pcs` is an example:

```
wait 2500
type lua\n
type print(sys.totalMemory())\n
wait 1000
key brk
screenshot out.png
quit 0
```

`type` takes the rest of the line with C escapes and holds Shift wherever the PicoCalc needs it.
`key` takes chords such as `ctrl+alt+del`, `shift+up`, `brk` or `f6`. `quit N` sets the exit code.

## How it works

```
 firmware UF2 ──> flash (QMI, ATRANS)             SD card image   LCD panel   keyboard
                     │                                 │             │           │
 emu/  Cortex-M33 x2 ─ bus ─ SIO/GPIO coprocessor   PL022 SPI ─ pins ─ PL022/PIO  DW I2C
        FPU, DCP       │     timers, watchdog       PIO, DMA                       │
                       │     clocks, PLLs, resets    PWM, UART                      │
                    boot ROM replacement                                          │
 sim/devices/   SD card (SPI mode)   ST7365P LCD controller   STM32 keyboard/battery controller
 frontend/      SDL2 window | headless + scripts + PNG
```

- **CPU.** Both Cortex-M33 cores are interpreted: ARMv8-M Mainline Thumb-2 with the DSP
  extension, the single-precision FPU, IT blocks, exclusive monitors shared between cores, and
  the full exception model (NVIC, priorities, BASEPRI, fault escalation, lazy FP stacking done
  eagerly). Each instruction is charged an approximate cycle count, and flash fetches go through
  a model of the 16 KB XIP cache, so code runs at about the speed it does on the chip.
- **Coprocessors.** The GPIO coprocessor is complete. The double-precision coprocessor (DCP) is
  modelled by what each canned operation computes rather than its internal datapath: operands are
  captured as doubles and the final read returns the IEEE result.
- **Boot ROM.** Raspberry Pi's ROM can't be redistributed, so `emu/rom.c` replaces it. It follows
  the real boot path (watchdog and reboot parameters, the picobin block loop, embedded and
  standalone partition tables, ATRANS address translation, load maps, BOOTSEL when nothing
  boots), runs core 1's launch handshake, and implements the ROM API the SDK and uf2loader call:
  flash operations, partition queries, `chain_image`, `reboot`, `get_sys_info` and so on.
- **Peripherals.** Register-level models of what the PicoCalc firmware uses: SIO (FIFOs,
  spinlocks, interpolators), IO bank and pads, TIMER0/1, watchdog, POWMAN's always-on timer,
  clocks/PLLs/tick generators, PL022 SPI, DesignWare I2C, PL011 UART, PIO (all three blocks,
  stepped instruction by instruction), 16-channel DMA, PWM, QMI direct mode with a W25Q flash and
  an APS6404 PSRAM, TRNG, OTP chip ID, boot RAM and boot locks.
- **Timing.** Emulated time is paced against the host clock. Buses move data at the rate the
  firmware programmed (a 10 kHz I2C poll of the keyboard takes as long as it does on the board).
- **Devices.** The LCD, SD card and keyboard controller answer whatever drives their pins: a
  PL022, a PIO program, or bit-banging. A PIO program that shifts one bit out per clock (the
  common LCD driver) is recognised and run in bulk.

## Status

Tested here with UF2s built from `../pcsh`, `../picocalc_lua` and `../picocalc-z80`:

| Firmware | Pico 2 | Pico 2 W | Pimoroni Plus 2 W |
| --- | --- | --- | --- |
| pcsh | shell, SD, Lua | boots; WiFi unavailable | boots with an 8 MB PSRAM heap; WiFi unavailable |
| picocalc_lua | boots, SD mounted | boots; WiFi unavailable | boots; WiFi unavailable |
| picocalc-z80 | boots to its menu | boots to its menu | boots, finds and benchmarks the PSRAM |
| uf2loader 2.4.1 | menu, flashing and chaining apps | not tried | not tried |

Not emulated yet, or not faithfully:

- **WiFi.** The CYW43439 on the W boards is absent, so `cyw43_arch_init()` fails.
- **Audio.** PWM and PIO audio run, but nothing is played yet.
- **Speed.** One busy core runs at full speed; firmware that keeps both cores busy (picocalc_lua,
  picocalc-z80) runs at about 55-60% of real time on a 2.9 GHz i7.
- **The PicoCalc mainboard's PSRAM**, which picocalc_lua reaches through PIO, is not attached.
- **USB** enumerates nothing: the controller's registers exist but no host is connected.
- **RP2040 boards** (Pico, Pico W) and RISC-V images are not supported.
- Secure state only: TrustZone, the MPU and the redundancy coprocessor accept their
  configuration but don't enforce it. OTP reads as unprogrammed apart from the chip ID.
- Assumptions about the keyboard controller I could not check against hardware are marked in
  `sim/devices/southbridge.c` (hold after 300 ms, release codes).

## Roadmap

1. CYW43439 emulation for the Pico 2 W and Pico Plus 2 W: the gSPI bus behind the PIO driver,
   firmware download, the SDPCM control channel (scan, join, events), and Ethernet frames bridged
   to a user-mode network stack on the host.
2. Audio from the PWM and PIO audio drivers, through SDL.
3. A faster CPU core (predecoded blocks) so dual-core firmware reaches real time.
4. The PicoCalc mainboard's PIO-attached PSRAM, and the RP2040 boards.
