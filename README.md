# picocalc_sim

A PicoCalc emulator for Linux and Windows that runs firmware as it ships: the same UF2 file you
would drop on the Pico's BOOTSEL drive or put in uf2loader's `pico2-apps` folder. macOS should
build but is untested. It emulates the RP2350 instruction by instruction, with the PicoCalc's LCD,
keyboard controller, SD card, audio circuit and PSRAM attached to its pins, so nothing has to be
rebuilt for the simulator.

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

### Windows

The Windows build is cross-compiled from Linux with MinGW-w64 (`apt install mingw-w64` on Debian
and Ubuntu):

```sh
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
cmake --build build-win -j
```

That gives `build-win/pcsim.exe` and `build-win/tools/mkcard/pcsim-mkcard.exe`. Each is a single
file that needs only DLLs Windows ships with, so it can be copied to any 64-bit Windows 10 or 11
machine. MSYS2's MinGW-w64 should build it natively on Windows the same way, but that is untested.

### Checking a change to the emulator

`PCSIM_STOP_AT_MS=MS` stops a run at exactly that much emulated time, and with `--trace` the exit
report ends with a digest of both cores' registers, cycle time and instruction counts and all of
SRAM. Unthrottled headless runs are deterministic, so a change that should not alter behaviour
(such as a faster CPU path) must leave every digest as it was, and the three ways the CPU can run
(native code, `PCSIM_NO_JIT=1` and `PCSIM_NO_BLOCKS=1`) must give the same one:

```sh
PCSIM_STOP_AT_MS=3000 build/pcsim --headless --unthrottled --trace --flash none --no-card fw.uf2 2>&1 | grep digest
```

### Measuring the hardware

`tools/psramprobe` is firmware for a real PicoCalc, built on its own with the Pico SDK (the first
line of its `CMakeLists.txt` says how). Started from uf2loader, it measures in PIO cycles how long
SCK and the mainboard PSRAM's output take to reach the PIO at 96 to 252 MHz, runs rp2040-psram's
read and write programs at several clocks and dividers, and writes the results to `PSPROBE.TXT` on
the card. The emulator's pin timing comes from one such report, and running the probe in the
emulator should give the same one.

### Continuous integration and releases

Pushing a `v*` tag runs `.forgejo/workflows/release.yml` in the shared `picocalc-builder` image.
It builds the tag, boots the latest pcsh, picocalc_lua, picocalc-z80 and ftpserver releases on
each board (`ci/boot-test.sh`) and transfers files to and from ftpserver over the emulated WiFi
(`ci/wifi-test.sh`), then cross-compiles the Windows build (`ci/build-windows.sh`) and runs the
same tests on it under Wine (`ci/wine-wrap.sh`). If they pass, it stages a draft release with
`pcsim-TAG-linux-x86_64.tar.gz` and `pcsim-TAG-windows-x86_64.zip`. Pushing a commit runs nothing;
`build.yml` runs the same tests on a branch when started by hand from the Actions page.
`ci/build-linux.sh` installs the X11, Wayland and audio headers SDL needs, so the Linux binaries
depend only on glibc 2.38 or newer (Ubuntu 24.04, Debian 13, Fedora 39) and can be built on a
headless machine and copied to a desktop.

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

On Windows that folder is `%LOCALAPPDATA%\pcsim`. Both programs are console programs, so run them
from a Command Prompt or PowerShell (or drop a UF2 onto `pcsim.exe`):

```bat
mkdir %LOCALAPPDATA%\pcsim
pcsim-mkcard.exe %LOCALAPPDATA%\pcsim\sd.img sd_files
pcsim.exe pcsh-pico2w-v0.1.2.uf2
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
(the default on Ubuntu, Debian and Fedora desktops). Windows sends them through its ICMP helper,
which any user may use.

Nothing outside can reach the firmware unless a port is forwarded. `--forward 2121:21` makes
127.0.0.1:2121 reach the firmware's port 21; `--forward *:2121:21` or `--forward ADDR:2121:21`
opens it to other machines too (Windows Defender Firewall asks first). Forwarding port 21 also
handles FTP's passive mode, so ftpserver works from an ordinary client:

```sh
build/pcsim --forward 2121:21 ../ftpserver/build-pico2w/picocalc-ftpserver.uf2
ftp -p 127.0.0.1 2121
```

`--no-wifi` leaves the radio out, as if it never answered. `PCSIM_WIFI_DEBUG=1` logs the driver's
ioctls, the chip's events and every bus command.

### Sound

Sound is what the mainboard's audio circuit makes of GP26 (left) and GP27 (right): per its
schematic, an RC low-pass at about 24 kHz and a coupling capacitor that takes off the DC, ahead of
the volume control, headphone jack and speakers. Whatever drives the pins is heard: the PWM block
(as a DAC on a fast carrier, or as a tone generator), a PIO program, or the CPU toggling them. The
window plays it about 50 ms behind the emulation; `--volume PCT` sets the loudness and `--mute`
leaves it out. `--wav FILE` records it in emulated time as 48 kHz stereo, so an unthrottled run
writes the same file every time:

```sh
PCSIM_STOP_AT_MS=5000 build/pcsim --headless --unthrottled --flash none --wav out.wav fw.uf2
```

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
| `--volume PCT` / `--mute` | loudness of the window's sound (default 50), or none |
| `--wav FILE` | record the sound as a 48 kHz stereo WAV, in emulated time |
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
                audio filters on GP26/GP27 ─> SDL sound, WAV file
                ESP-PSRAM64H PSRAM on GP2-5 and GP20/21
 frontend/      SDL2 window | headless + scripts + PNG
```

- **CPU.** Both Cortex-M33 cores are interpreted: ARMv8-M Mainline Thumb-2 with the DSP
  extension, the single-precision FPU, IT blocks, exclusive monitors shared between cores, and
  the full exception model (NVIC, priorities, BASEPRI, fault escalation, lazy FP stacking done
  eagerly). Each instruction is charged an approximate cycle count, and flash fetches go through
  a model of the 16 KB XIP cache, so code runs at about the speed it does on the chip. Runs of
  straight-line code are decoded once into blocks of small handlers (`emu/block.inc`) and reused
  while the code they came from is unchanged, and on x86-64 hosts each block is then translated
  to native code (`emu/jit.inc`). Both behave exactly as stepping one instruction at a time:
  `PCSIM_NO_JIT=1` keeps to the block handlers and `PCSIM_NO_BLOCKS=1` steps.
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
  stepped instruction by instruction on whole system clock cycles, so a fractional divider gives
  uneven ones as on the chip, except that the counting loop of pico-examples' PWM program, which
  PIO audio drivers use, skips ahead to its next edge), 16-channel DMA, PWM, QMI
  direct mode with a W25Q flash and an APS6404 PSRAM, TRNG, OTP chip ID, boot RAM and boot locks.
- **Timing.** Emulated time is paced against the host clock. Buses move data at the rate the
  firmware programmed (a 10 kHz I2C poll of the keyboard takes as long as it does on the board).
- **Pin timing.** A pin's change reaches the PIO's inputs a cycle plus the pads' delays later
  (6.2 to 6.8 ns there and back with the default 4 mA slow-slew pad on a PicoCalc, less at higher
  drive or fast slew), then through the 2-cycle input synchroniser unless it is bypassed, and a
  device answers an edge as late as the real part would. A PIO driver that samples too early or
  too late therefore reads the same wrong bits here as on the board.
- **Devices.** The LCD, SD card and keyboard controller answer whatever drives their pins: a
  PL022, a PIO program, or bit-banging. A PIO program that shifts one bit out per clock (the
  common LCD driver) is recognised and run in bulk. The mainboard's ESP-PSRAM64H is modelled at its
  pins, SPI and QPI, answering 3.8 ns (rising) or 5.5 ns (falling) after the clock edge as a
  PicoCalc's did, and it logs a warning when firmware goes past the datasheet's clock limits
  (33 MHz for `0x03`, 133 MHz, 84 MHz across a 1 KB page) or holds CE# low beyond 8 µs.
- **WiFi.** The SDK drives the CYW43439 over a half-duplex SPI from a PIO program, which is
  recognised and exchanges bits with the chip model directly. The model implements the gSPI bus
  registers and the backplane the radio firmware is downloaded into (the firmware in the UF2 is
  stored but never run), then serves the SDPCM channels that firmware would: ioctls and iovars on
  the control channel, scan, join and link events, and Ethernet frames, with the bus credits that
  pace the host. Frames go to `sim/net`, which answers ARP, DHCP, DNS and ping itself, relays UDP
  through a host socket per firmware port, and terminates TCP at a copy of lwIP running as the
  far end of every connection, each relayed over a host socket.
- **Sound.** `emu/pinwave.c` records the waveform of the audio pins without touching the chip: a
  PWM output as its slice settings, so a fast carrier costs nothing per cycle, and any other
  driver as edges stamped with the time the core or PIO state machine made them. It integrates
  that into the fraction of each 1/192000 s the pin was high. `sim/devices/audio.c` puts that
  through the board's filters, worked out from its resistors and capacitors, and resamples it to
  48 kHz. A PWM carrier above 20 kHz is taken as its average, since it is inaudible.

## Status

Tested here with UF2s built from `../pcsh`, `../picocalc_lua`, `../picocalc-z80` and
`../ftpserver`, and with ClockworkPi's PicoMite build:

| Firmware | Pico 2 | Pico 2 W | Pimoroni Plus 2 W |
| --- | --- | --- | --- |
| pcsh | shell, SD, Lua | WiFi scan and join, DHCP, DNS, NTP, HTTP, ping to the gateway | boots with an 8 MB PSRAM heap |
| picocalc_lua | boots, SD mounted, `sound` notes, `psram` on the mainboard PSRAM | WiFi scan and join, `psram` | boots |
| picocalc-z80 | boots to its menu | boots to its menu | boots, finds and benchmarks the PSRAM |
| ftpserver | (no Pico 2 build) | FTP through a forward: login, list, transfers | same |
| uf2loader 2.4.1 | menu, flashing and chaining apps | not tried | not tried |
| PicoMite 6.00.02RC23 | MMBasic prompt after its first-boot flash format (about 20 s), `PLAY TONE` | not tried | same, on the non-W Plus 2 |

Not emulated yet, or not faithfully:

- **WiFi** is a virtual network rather than a radio. Access point mode is not modelled,
  broadcasts and multicasts (mDNS, SSDP) stay inside, it is IPv4 only, and the chip's Bluetooth
  is absent. Script `wait`s are host time, so a much slower host (or a sanitizer build)
  can type ahead of the firmware.
- **Sound** is the line signal the headphone jack gets. The speaker amplifiers' 226 Hz input
  high-pass and the speakers themselves are left out, and the volume knob is `--volume`. The
  window's sound breaks up when the emulation can't keep up with real time.
- **Speed.** On a 2.9 GHz i7, firmware that keeps both cores busy (picocalc_lua, picocalc-z80)
  runs at 1.7 to 2 times real time, and a Lua loop at about 2.3 times. Hosts other than x86-64 use
  the block handlers, at about 80% of real time for the same firmware.
- **Mainboard PSRAM timing** was measured on one PicoCalc, with the default 4 mA slow-slew pads and
  at 12 mA fast slew; other pad settings are interpolated, and quad mode is unmeasured. A read
  that lands within a fraction of a nanosecond of a change, which varies from read to read on the
  board, gives the same answer every time here.
- **USB** enumerates nothing: the controller's registers exist but no host is connected.
- **RP2040 boards** (Pico, Pico W) and RISC-V images are not supported.
- **Windows:** `pcsim-mkcard.exe` reads host file names in the system code page, so files whose
  names are not plain ASCII may not copy.
- Secure state only: TrustZone, the MPU and the redundancy coprocessor accept their
  configuration but don't enforce it. OTP reads as unprogrammed apart from the chip ID.
- Assumptions about the keyboard controller I could not check against hardware are marked in
  `sim/devices/southbridge.c` (hold after 300 ms, release codes).

## Roadmap

1. The RP2040 boards (Pico, and Pico W with the same radio model).
2. A macOS build, and optionally bridged networking so the firmware gets an address on the LAN.
