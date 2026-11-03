# picocalc_sim

A PicoCalc emulator for Linux that runs firmware as it ships: the same UF2 file you would drop on
the Pico's BOOTSEL drive or put in uf2loader's `pico2-apps` folder. macOS should build but is
untested; Windows is not ported yet. It emulates the RP2350 instruction by instruction, with the
PicoCalc's LCD, keyboard controller and SD card attached to its pins, so nothing has to be rebuilt
for the simulator.

Boards: **Raspberry Pi Pico 2**, **Pico 2 W** and **Pimoroni Pico Plus 2 / 2 W** (16 MB flash,
8 MB PSRAM). On the W boards the CYW43439 radio is emulated too: firmware scans, joins a virtual
access point and reaches the network through a router built into the emulator (see [WiFi](#wifi)).

## Building

You need CMake 3.16+ and a C compiler. Nothing from the Pico SDK or an ARM toolchain is needed.

```sh
cmake -S . -B build
cmake --build build -j
```

If SDL2 is not installed the build downloads SDL 2.30.9 (hash-pinned) and links it statically.
Installing the system package (`libsdl2-dev`, `brew install sdl2`) skips that; `-DPCSIM_SDL=OFF`
builds a headless-only binary. FatFs, used by the card-image tool, comes from a sibling
`ftpserver`, `picocalc_lua` or `picocalc-z80` checkout if there is one, and lwIP, used by the
emulated network, from a sibling `pico-sdk` (or `PICO_SDK_PATH`); either is fetched otherwise.

The build produces `build/pcsim`, the emulator, and `build/tools/mkcard/pcsim-mkcard`, which makes
SD card images.

### Continuous integration and releases

`.forgejo/workflows/build.yml` builds every pushed branch in the shared `picocalc-builder` image
and boots the latest pcsh, picocalc_lua, picocalc-z80 and ftpserver releases on each board
(`ci/boot-test.sh`), then transfers files to and from ftpserver over the emulated WiFi
(`ci/wifi-test.sh`). Pushing a `v*` tag runs `release.yml`, which stages a draft release with a
`pcsim-TAG-linux-x86_64.tar.gz`. `ci/build-linux.sh` installs the X11, Wayland and audio headers
SDL needs, so the binaries depend only on glibc 2.38 or newer (Ubuntu 24.04, Debian 13,
Fedora 39) and can be built on a headless machine and copied to a desktop.

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

### WiFi

The W boards' firmware sees a CYW43439 that answers the SDK's driver as the real chip does, from
the firmware download onwards. Behind it is a private network the emulator routes for:

| Address | What it is |
| --- | --- |
| 10.0.2.15 | the PicoCalc, handed out by DHCP |
| 10.0.2.2 | the gateway, which is the host: a connection to 10.0.2.2:PORT reaches the host's 127.0.0.1:PORT |
| 10.0.2.3 | DNS, answered from the host's own resolver |

Without `--wifi-ap`, a scan finds one network, `PicoCalc-Sim`, and any network name and password
the firmware tries will join, so settings already saved on a card just work. With
`--wifi-ap SSID[:PASSWORD]` (repeat it for more), only those networks exist and a join behaves as
on real hardware: a wrong password fails authentication and an unknown name is not found. A
network given without a password is open.

TCP and UDP from the firmware go out through ordinary sockets on the host, as from behind a home
router. Pings to 10.0.2.2 and 10.0.2.3 are always answered; pings beyond need unprivileged ICMP
sockets, which macOS allows and Linux allows when `net.ipv4.ping_group_range` covers your group
(the default on Ubuntu, Debian and Fedora desktops).

Nothing outside can reach the firmware unless a port is forwarded. `--forward 2121:21` makes
127.0.0.1:2121 reach the firmware's port 21; `--forward *:2121:21` or `--forward ADDR:2121:21`
opens it to other machines too. Forwarding port 21 also handles FTP's passive mode, so ftpserver
works from an ordinary client:

```sh
build/pcsim --forward 2121:21 ../ftpserver/build-pico2w/picocalc-ftpserver.uf2
ftp -p 127.0.0.1 2121
```

`--no-wifi` leaves the radio out, as if it never answered. `PCSIM_WIFI_DEBUG=1` logs the driver's
ioctls, the chip's events and every bus command.

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
| `--wifi-ap SSID[:PASSWORD]` | a virtual access point (repeatable); without one, any network joins |
| `--forward [ADDR:]HOSTPORT:PORT` | let host connections to HOSTPORT reach the firmware's TCP PORT (repeatable) |
| `--no-wifi` | leave the W boards' radio out |
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
                    boot ROM replacement   PIO gSPI ─ CYW43439 (emu/cyw43.c)       │
                                                         │ Ethernet frames         │
 sim/net/       DHCP, DNS, UDP and ping NAT; TCP ends at lwIP, relayed to host sockets
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
- **WiFi.** The SDK drives the CYW43439 over a half-duplex SPI from a PIO program, which is
  recognised and exchanges bits with the chip model directly. The model implements the gSPI bus
  registers and the backplane the radio firmware is downloaded into (the firmware in the UF2 is
  stored but never run), then serves the SDPCM channels that firmware would: ioctls and iovars on
  the control channel, scan, join and link events, and Ethernet frames, with the bus credits that
  pace the host. Frames go to `sim/net`, which answers ARP, DHCP, DNS and ping itself, relays UDP
  through a host socket per firmware port, and terminates TCP at a copy of lwIP running as the
  far end of every connection, each relayed over a host socket.

## Status

Tested here with UF2s built from `../pcsh`, `../picocalc_lua`, `../picocalc-z80` and
`../ftpserver`:

| Firmware | Pico 2 | Pico 2 W | Pimoroni Plus 2 W |
| --- | --- | --- | --- |
| pcsh | shell, SD, Lua | WiFi scan and join, DHCP, DNS, NTP, HTTP, ping to the gateway | boots with an 8 MB PSRAM heap |
| picocalc_lua | boots, SD mounted | WiFi scan and join | boots |
| picocalc-z80 | boots to its menu | boots to its menu | boots, finds and benchmarks the PSRAM |
| ftpserver | (no Pico 2 build) | FTP through a forward: login, list, transfers | same |
| uf2loader 2.4.1 | menu, flashing and chaining apps | not tried | not tried |

Not emulated yet, or not faithfully:

- **WiFi** is a virtual network rather than a radio. Access point mode is not modelled,
  broadcasts and multicasts (mDNS, SSDP) stay inside, it is IPv4 only, and the chip's Bluetooth
  is absent. Script `wait`s are host time, so a much slower host (or a sanitizer build)
  can type ahead of the firmware.
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

1. Audio from the PWM and PIO audio drivers, through SDL.
2. A faster CPU core (predecoded blocks) so dual-core firmware reaches real time.
3. The PicoCalc mainboard's PIO-attached PSRAM, and the RP2040 boards (Pico W with the same
   radio model).
4. Windows (MinGW) and macOS builds, and optionally bridged networking so the firmware gets an
   address on the LAN.
