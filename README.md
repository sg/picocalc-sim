# picocalc-sim

A PicoCalc simulator/emulator for Linux and Windows that runs the same UF2 files you
would drop on the Pico's BOOTSEL drive or put in uf2loader's `pico1-apps` or `pico2-apps` folder. 
It emulates the RP2350 or RP2040 instruction by instruction, with the
PicoCalc's LCD, keyboard controller, SD card, audio circuit and PSRAM attached to its pins, so
the simulator should reproduce how the firmware would run on the real hardware.

Boards: **Raspberry Pi Pico 2**, **Pico 2 W** and **Pimoroni Pico Plus 2 / 2 W** (16 MB flash,
8 MB PSRAM), and the RP2040 **Raspberry Pi Pico** and **Pico W**, which need the RP2040's boot ROM
(see [RP2040 boards](#rp2040-boards)). On the W boards the CYW43439 radio is emulated too: firmware
scans, joins a virtual access point and reaches the network through a router built into the
emulator (see [WiFi](#wifi)).

## Running firmware

```sh
./picocalc-sim picocalc-app.uf2
```

A UF2 on the command line is written into flash as BOOTSEL's drive would write it, then the board
boots. Without one, the board boots whatever its flash already holds, like switching the PicoCalc
on. The board is taken from the image's binary info (`pico2`, `pico2_w`,
`pimoroni_pico_plus2_w_rp2350`, `pico`, `pico_w`); passing the `--board <board-name>` option overrides it. When 
a board isn't specified in either, the application uses the board it last ran (see [Settings file](#settings-file)), and otherwise
`pico2`.

Each board's flash persists in `~/.local/share/picocalc-sim/BOARD.flash` (`--flash FILE` to choose,
`--flash none` to keep it only in memory until picocalc-sim exits, `--erase-flash` to start blank).
The SD card is loaded from `~/.local/share/picocalc-sim/sd.img` if it exists, or `--card IMAGE`:

```sh
./picocalc-sim-mkcard ~/.local/share/picocalc-sim/sd.img path/to/sd_files
```

On Windows that folder is `%LOCALAPPDATA%\picocalc-sim`. `picocalc-sim.exe` is a Windows GUI program:
open it by double-clicking in Explorer, or drop a UF2 onto it, and only its window appears. A Command Prompt or PowerShell neither
waits for a windowed program nor shows what it prints, so `picocalc-sim-console.exe` is the same
program built for a console, for headless runs, `--console` and picocalc-sim's messages.
`picocalc-sim-mkcard.exe` is a console program as well:

```bat
mkdir %LOCALAPPDATA%\picocalc-sim
picocalc-sim-mkcard.exe %LOCALAPPDATA%\picocalc-sim\sd.img path\to\sd_files
picocalc-sim-console.exe pcsh-pico2w-v0.1.2.uf2
```

`picocalc-sim-mkcard IMAGE [-s MB] [-l LABEL] [DIR...]` formats an MBR/FAT32 card (512 MB by
default) and copies the contents of each specified directory to the root of the image. Files the firmware writes persist in the
image.

### RP2040 boards

The RP2040 runs its own boot ROM, which reads boot2 from flash, checks it, and runs it to set up
the flash for execute-in-place; firmware also calls the ROM for flash writes, memory copies and
all of its floating-point maths. Raspberry Pi publishes the exact ROM of each chip revision, but
its floating-point code is licensed only for use on an RP2040, so it is not included here.
Download `b2.elf` (the revision in current Picos) from
[the pico-bootrom-rp2040 releases](https://github.com/raspberrypi/pico-bootrom-rp2040/releases)
and either save it as `~/.local/share/picocalc-sim/rp2040-rom.elf` or pass it each time:

```sh
./picocalc-sim --rom b2.elf picocalc-app.uf2
```

A 16 KB raw image of the ROM (`rp2040-rom.bin`, or a dump made with
`picotool save -r 0x0 0x4000`) works too.

### RP2350 boot ROM

The RP2350 ROMs are BSD-3 licensed so the A4 and A2 releases are built into picocalc-sim, from
[roms/rp2350](roms/rp2350), and A4 is the default. Specifying `--rp2350-rom <A2|A4|stand-in>`, or the RP2350 ROM field in
Settings, picks another: `A2`, `A4`, `stand-in` for picocalc-sim's own stand-in for the ROM (see
[How it works](#how-it-works)), or the path of another `bootrom-combined.bin` from Raspberry Pi's
[releases](https://github.com/raspberrypi/pico-bootrom-rp2350/releases).

```sh
./picocalc-sim --rp2350-rom A2 picocalc-app.uf2
```

Images in flash or RAM and uf2loader work on it. Its USB boot (which the BOOTSEL button, a reboot
into BOOTSEL and an empty flash all lead to) runs in Non-secure state as on the chip, and the host 
operating system enumerates it as the RP2350 Boot device and mounts its drive, onto which Flash UF2 then
copies an image for the ROM to write (see [USB](#usb)). picocalc-sim's menu bar and title will indicate BOOTSEL
while it runs, and F12 resets the board. The emulated chip becomes the "stepping" the ROM was built 
for: an A2 with the A2 ROM, and an A3 with the A3 or A4 ROM, since A4 changed only the ROM.

### uf2loader

pelrun's [uf2loader](https://github.com/pelrun/uf2loader) works as it does on a real PicoCalc:

```sh
mkdir -p sd_files/pico2-apps
cp BOOT2350.uf2 sd_files/BOOT2350.UF2
cp picocalc-app.uf2 another-app.uf2 sd_files/pico2-apps/
./picocalc-sim-mkcard ~/.local/share/picocalc-sim/sd.img sd_files
./picocalc-sim --board pico2 bootloader_pico2.uf2
```

With no app in flash the loader opens its menu; choosing a UF2 flashes it into the app partition and
boots it. Later runs (`./picocalc-sim` with no UF2) boot the app directly. For the menu instead,
hold F1 (or Up) while choosing Machine › Restart, as when switching the PicoCalc on with it held, or
press F12 and hold F1 straight after; held before F12, the key reaches the running app, not the
loader. The RP2040 build works the same way, with `BOOT2040.UF2`, a `pico1-apps` folder and `--board
pico bootloader_pico.uf2`.

BlairLeduc's [PicoCalc shell](https://github.com/BlairLeduc/picocalc-shell) takes the uf2loader menu's place
on RP2350 boards: rename the card's `BOOT2350.UF2` to `BOOT2350.ORG`, copy the shell there as
`BOOT2350.UF2`, and type an app's name at its prompt to flash it from `pico2-apps` and boot it.

A UF2 for RAM rather than flash (such as `BOOT2350.UF2` itself) is loaded into SRAM after
power-on, as BOOTSEL's drive would, and the board resets into it.

### WiFi

The W boards' firmware sees an emulated CYW43439 that answers the SDK's driver as the real chip does, from
the firmware download onwards. Behind it is a private network the emulator routes for:

| Address | What it is |
| --- | --- |
| 10.0.2.15 | the PicoCalc, handed out by DHCP just as a WiFi access point would |
| 10.0.2.2 | the gateway, which is the host: a connection to 10.0.2.2:PORT reaches the host's 127.0.0.1:PORT |
| 10.0.2.3 | DNS, answered from the host's own resolver |

Without `--wifi-ap`, a scan finds one network, `PicoCalc-Sim`, and any network name and password
the firmware tries will join, so settings already saved on a card just work. With
`--wifi-ap SSID[:PASSWORD]` (repeat it for more), only those networks exist and a join behaves as
on real hardware: a wrong password fails authentication and an unknown name is not found. A
network given without a password is open. These are provided so that firmware apps can be tested 
to confirm that they will work properly when trying to connect to a real Wifi AP from the PicoCalc 
device.

TCP and UDP from the firmware go out through ordinary sockets on the host, as from behind a home
router. Pings to 10.0.2.2 and 10.0.2.3 are always answered. Pings beyond need unprivileged ICMP
sockets, which macOS allows and Linux allows when `net.ipv4.ping_group_range` covers your group
(the default on Ubuntu, Debian and Fedora desktops). Windows sends them through its ICMP helper,
which any user may use.

Nothing outside can reach the firmware unless a port is forwarded. `--forward 2121:21` makes
127.0.0.1:2121 reach the firmware's port 21; `--forward *:2121:21` or `--forward ADDR:2121:21`
opens it to other machines too (Windows Defender Firewall asks first). Forwarding port 21 also
handles FTP's passive mode, so running an FTP server app works from an ordinary client:

```sh
./picocalc-sim --forward 2121:21 picocalc-ftpserver.uf2
ftp -p 127.0.0.1 2121
```

`--no-wifi` leaves the radio out, as if it never answered. `PICOCALC_SIM_WIFI_DEBUG=1` logs the
driver's ioctls, the chip's events and every bus command.

### USB

`--usb`, or selecting "USB cable plugged into emulated host" in Settings, connects the board's USB port to a
USB host emulated inside picocalc-sim. The computer picocalc-sim runs on does not see the device;
[USB/IP](#usbip) gives it to that computer's own USB stack.
This USB "cable" emulates VBUS (power), which the board senses (GPIO24 on the Pico, Pico 2 and Pimoroni Plus 2, the
radio's WL_GPIO2 on the W boards). When the firmware connects its USB device, the host waits for the
connection to settle, resets the port and enumerates the device as Linux does: it reads the device,
configuration and string descriptors, gives the device an address and sets its configuration. It
logs what it found and then keeps the bus awake with a start-of-frame packet every millisecond:

```
usb: 2e8a:000f "RP2350 Boot" from "Raspberry Pi", serial "E3502C418F3B1D5A", USB 2.10; interfaces 0 mass storage, 1 vendor-specific
```

The above log entry indicates Raspberry Pi's RP2350 ROM in BOOTSEL. the RP2040's ROM gives `2e8a:0003 "RP2 Boot"`, and
firmware built with the SDK's USB stdio gives its CDC serial port. A device that disconnects, as
across a reset, is enumerated again when it reconnects. A device that is slow to answer as it
starts, such as MicroPython formatting its flash on first boot, is retried as Linux would, up to 8
times. The cable can be plugged and unplugged while the firmware runs, from Settings or a script's
`usb on` and `usb off`. `PICOCALC_SIM_USB_DEBUG=1` logs each request and each packet on the serial
port, with the emulated time.

`--usb-console` plugs the cable in and makes the terminal picocalc-sim runs in a serial terminal on
the firmware's USB serial port, the CDC ACM interface that the SDK's USB stdio, TinyUSB's CDC class
and MicroPython present. The host opens the port as Linux does when a program opens it: 115200
baud, 8N1, then DTR and RTS, which the SDK's USB stdio waits for before it sends anything. What the
firmware sends is printed, and what is typed goes to it key by key and unechoed, since the firmware
echoes it. Ctrl+C goes to the firmware. Ctrl+] quits picocalc-sim. Input piped in rather than typed
waits for the port to open, and output to a file or pipe loses its carriage returns:

```sh
./picocalc-sim --usb-console RPI_PICO2-20260824-v1.29.0.uf2
>>> print(6*7)
42
```

In the picocalc-sim appliction, View › USB serial terminal (or `--usb-terminal`) opens the same terminal beside the
LCD, the window widening to make room for it, and works on Windows too, where `picocalc-sim.exe`
has no console. It is an 80 by 24 VT100 screen with 2000 lines of history, colours, and the cursor
keys, editing keys and cursor position reports that line editors and full-screen programs use.
Clicking it gives it the keyboard, so keys go to the firmware's serial port rather than the PicoCalc
keyboard, until the LCD is clicked. Ctrl+C reaches the firmware; Paste (Ctrl+Shift+V) types the
clipboard, each line ended with Enter, and Copy (Ctrl+Shift+C) takes everything the terminal holds.
It says when the cable is unplugged, with a button to plug it in, or when the firmware has no
serial port. Closing it drops DTR, as closing a terminal program does.

A script's `usb send TEXT` sends text to the port, with C escapes (`\r` for Enter). With no terminal
on it the port stays closed, as on a computer with no terminal program open. Firmware that puts its
console on UART0 instead, as the PicoCalc's PicoMite build does for the PicoCalc's own USB-C serial
port, prints nothing here; `--console` shows UART0.

A drive on the device, a mass storage interface such as the BOOTSEL drive of Raspberry Pi's boot
ROMs, is mounted as an OS would mount it: the host asks what it is, waits for it to be ready, reads
its capacity and FAT volume, and logs the volume's label and INFO_UF2.TXT:

```
usb: drive "RP2350" (RPI RP2350, 128 MB), INFO_UF2.TXT "UF2 Bootloader v1.1; Model: Raspberry Pi RP2350; Board-ID: RP2350"
```

While it is mounted, Flash UF2 and dropping a UF2 on the window copy the file onto it, as dragging it
onto the drive on a computer would, and the boot ROM writes the flash itself and restarts into it.
A script's `usb copy FILE` does the same once the drive has mounted. A UF2 the ROM does not take,
such as one built for the other chip, leaves it in BOOTSEL, and the host says so. Without a mounted
drive, Flash UF2 writes the flash directly as before. The ROMs' PICOBOOT interface goes unused
here; over USB/IP it is picotool's.

#### USB/IP

`--usbip [ADDR:]PORT`, or the USB/IP field in Settings, exports the device to a computer's own USB
stack over the network, as Linux's usbipd does: on that computer, `usbip attach -r HOST -b 1-1`
(on Linux, as root and with the vhci-hcd module loaded; on Windows, with usbip-win2) and its drivers
take the device as if it were plugged in, for picotool, a serial terminal or the file manager to
use. It listens on 127.0.0.1 unless ADDR is given (`*` for every interface), and plugs the cable
in:

```sh
./picocalc-sim --flash none --erase-flash --usbip 3240
sudo modprobe vhci-hcd
sudo usbip --tcp-port 3240 attach -r 127.0.0.1 -b 1-1
```

`usbip list -r HOST` shows what is exported, and needs no root. The host on the cable stays the
device's host: it keeps the bus awake and runs each URB the computer sends as transactions on the
emulated bus, and does itself what Linux's server does itself, setting configurations and
interfaces, clearing halts and resetting the device, so the data toggles stay in step. On import it
resets the device so that it starts afresh for the computer, and its own serial port and drive
stand aside until the computer lets go, when the device is enumerated again for them. A device that
disconnects, as one restarting into a new UF2 does, ends the import as unplugging it would; the
computer then attaches again. One computer at a time can have it, isochronous transfers are not
carried, and `PICOCALC_SIM_USB_DEBUG=1` logs each URB.

Attached to Linux 6.8 this way, the Pico 2's and Pico's boot ROMs mount as drives that a UF2 can be
copied onto, picotool reads and loads them over PICOBOOT and sends running firmware back to
BOOTSEL, and `tests/usbserial` appears as `/dev/ttyACM0`.

### Sound

Sound is what the mainboard's audio circuit makes of GP26 (left) and GP27 (right): per its
schematic, an RC low-pass at about 24 kHz and a coupling capacitor that takes off the DC, ahead of
the volume control, headphone jack and speakers. Whatever drives the pins is heard: the PWM block
(as a DAC on a fast carrier, or as a tone generator), a PIO program, or the CPU toggling them. The
window plays it about 50 ms behind the emulation; `--volume PCT` sets the loudness and `--mute`
leaves it out. `--wav FILE` records it in emulated time as 48 kHz stereo, so an unthrottled run
writes the same file every time:

```sh
PICOCALC_SIM_STOP_AT_MS=5000 ./picocalc-sim --headless --unthrottled --flash none --wav out.wav fw.uf2
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
| **F11** | screenshot of the LCD, into the screenshot folder (emulator only) |
| **F12** | the Pico's reset; **Shift+F12** resets into BOOTSEL |

Drop a UF2 file on the window to flash it, or a `.img` file to insert it as the SD card. Holding a
key currently does not auto-repeat, as on the real keyboard. Key mapping assumes a US layout. On Windows,
characters that Remote Desktop and similar tools send as Unicode rather than as keys are typed with
the PicoCalc keys for them. When the
firmware powers the PicoCalc off, any key powers it back on. A key held while the machine restarts
is still down as it powers on. Keys go to the PicoCalc except while one of picocalc-sim's own text
fields has them.

### Menus and settings

The window's menu bar covers what the options below set at launch, so most of them can be changed
without quitting:

- **Machine**: Restart, the reset button (F12), reset into BOOTSEL (Shift+F12), the power button,
  Flash UF2 (as BOOTSEL's drive would), Erase flash and restart, Settings, and Quit.
- **SD card**: insert an image, or eject the one in the slot. A card has to be ejected before
  another goes in, as the firmware may still hold the old card's tables.
- **Sound**: mute, volume, and recording to a WAV file from now on.
- **View**: window scale from 1x to 4x in half steps, screenshot (F11), a log of picocalc-sim's
  messages, and the USB serial terminal (see [USB](#usb)). The LCD fills the window as it is
  resized: at a whole multiple of its 320 pixels each LCD pixel is a square of screen pixels, and at
  any other size SDL's pixel-art scaling keeps every LCD pixel the same size, blending only the
  screen pixel where two meet (SDL's software renderer, used where there is no GPU, has no such
  scaling and draws some LCD pixels a screen pixel wider than others). A screenshot is saved as
  `picocalc-DATE-TIME-N.png`, and a notice over the LCD gives its file name and full path for a few
  seconds, or why it could not be saved.

Settings, the Log, Keyboard and About open in windows of their own that stay above the PicoCalc's,
beside it where the screen has room, so they are as big as they need to be at any scale. Settings
has the board, the RP2040 and RP2350 boot ROMs, where the flash is kept, fast transfers, tracing,
the radio, access points and port forwards, which take effect at the next restart, and throttling,
the UART0 console, the USB cable, battery, sound and the screenshot folder, which take effect at
once.
Restart is the PicoCalc's power switch: each board's flash keeps what it holds (one board's flash is
not another's, as if the Pico were swapped), and the UF2, `--erase-flash` and `--wav` from the
command line apply only to the first start. If the machine cannot start (e.g., a missing boot ROM
or a port already in use) the window shows why and waits for the settings to be fixed.

### Settings file

What is changed in the window is saved to `settings.ini` in the data folder
(`~/.local/share/picocalc-sim`, or `%LOCALAPPDATA%\picocalc-sim` on Windows) and used at its next
launch, along with the board it last ran and the card last inserted or ejected. Each line is a long
option without its dashes, so the file can be edited by hand:

```ini
board = pico2_w
card = /home/me/cards/lua.img
wifi-ap = HomeNet:secret123
forward = 2121:21
volume = 30
mute = off
scale = 3
```

The keys are `board`, `rom`, `rp2350-rom` (`A4`, `A2`, `stand-in` or a file), `flash` (a file,
`none` or `default`), `card` (an image or `none`), `fast`, `unthrottled`, `trace`, `console`,
`no-wifi`, `wifi-ap` and `forward` (repeated for each), `usb`, `usb-terminal`, `usbip`, `battery`,
`charging`, `volume`, `mute`, `scale` and `screenshot-dir`; switches take `on` or `off`. An option
given on the command line wins for that run and is not written to the file unless it is then changed
in the window; the board is the exception, as the one last run is always kept. picocalc-sim rewrites
the file as settings change, keeping keys it does not know but not comments. Headless runs ignore
it, so scripts and CI see only their own options.

### Options

| Option | Effect |
| --- | --- |
| `-b`, `--board NAME` | `pico2`, `pico2_w`, `pimoroni_pico_plus2_w`, `pimoroni_pico_plus2`, `pico` or `pico_w` |
| `--rom FILE` | the RP2040 boot ROM, as an ELF file or a 16 KB image (see [RP2040 boards](#rp2040-boards)) |
| `--rp2350-rom A4\|A2\|stand-in\|FILE` | the RP2350 boot ROM: Raspberry Pi's A4 (the default) or A2, built in, picocalc-sim's stand-in, or a file (see [RP2350 boot ROM](#rp2350-boot-rom)) |
| `-f`, `--flash FILE` | flash image to use and keep |
| `-c`, `--card IMAGE` / `--no-card` | SD card image, or an empty slot |
| `--console` | print UART0 output, as a serial adapter on the PicoCalc's header would show |
| `--fast` | SPI, I2C and UART transfers complete instantly |
| `--unthrottled` | run as fast as the host allows instead of at device speed |
| `--battery PCT`, `--charging` | what the keyboard controller reports |
| `--scale N` | window scale, 1 to 8 in any step such as 1.5 (default 2) |
| `--volume PCT` / `--mute` | loudness of the window's sound (default 50), or none |
| `--wav FILE` | record the sound as a 48 kHz stereo WAV, in emulated time |
| `--wifi-ap SSID[:PASSWORD]` | a virtual access point (repeatable); without one, any network joins |
| `--forward [ADDR:]HOSTPORT:PORT` | let host connections to HOSTPORT reach the firmware's TCP PORT (repeatable) |
| `--no-wifi` | leave the W boards' radio out |
| `--usb` | plug the board's USB port into a host that enumerates the firmware's device (see [USB](#usb)) |
| `--usb-console` | `--usb`, with the terminal on the firmware's USB serial port; Ctrl+] quits |
| `--usb-terminal` | in the window, the terminal on that port beside the LCD |
| `--usbip [ADDR:]PORT` | `--usb`, with the device exported over USB/IP for `usbip attach -r HOST -b 1-1` |
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
`key` takes chords such as `ctrl+alt+del`, `shift+up`, `brk` or `f6`. `restart` stops the
machine and starts it again as a fresh launch with the same options would, so the UF2 given on
the command line is written again and the card stays in the slot (unlike the menu's Restart).
`usb on` and `usb off` plug the USB cable in and pull it out, `usb send TEXT` sends text (with
`type`'s escapes, plus `\r`) to the firmware's USB serial port, which `--usb-console` opens, and
`usb copy FILE` copies a UF2 onto a boot ROM's drive once it is mounted. `quit N` sets the exit code.

## How it works

```
 firmware UF2 ──> flash (QMI, ATRANS)             SD card image   LCD panel   keyboard
                     │                                 │             │           │
 emu/  Cortex-M33 x2 ─ bus ─ SIO/GPIO coprocessor   PL022 SPI ─ pins ─ PL022/PIO  DW I2C
        FPU, DCP       │     timers, watchdog       PIO, DMA                       │
                       │     clocks, PLLs, resets    PWM, UART                      │
                    boot ROM or stand-in   PIO gSPI ─ CYW43439 (emu/cyw43.c)       │
                                                         │ Ethernet frames         │
 sim/net/       DHCP, DNS, UDP and ping NAT; TCP ends at lwIP, relayed to host sockets
 sim/devices/   SD card (SPI mode)   ST7365P LCD controller   STM32 keyboard/battery controller
                audio filters on GP26/GP27 ─> SDL sound, WAV file
                ESP-PSRAM64H PSRAM on GP2-5 and GP20/21
 frontend/      SDL3 window with Dear ImGui menus | headless + scripts + PNG
```

- **CPU.** Both Cortex-M33 cores are interpreted: ARMv8-M Mainline Thumb-2 with the DSP extension,
  the single-precision FPU, IT blocks, exclusive monitors shared between cores, and the full
  exception model (NVIC, priorities, BASEPRI, fault escalation, lazy FP stacking done eagerly). Each
  instruction is charged an approximate cycle count, and flash fetches go through a model of the 16
  KB XIP cache, so code runs at about the speed it does on the chip. Runs of straight-line code are
  decoded once into blocks of small handlers (`emu/block.inc`) and reused while the code they came
  from is unchanged, and on x86-64 hosts each block is then translated to native code
  (`emu/jit.inc`). Both behave exactly as stepping one instruction at a time:
  `PICOCALC_SIM_NO_JIT=1` keeps to the block handlers and `PICOCALC_SIM_NO_BLOCKS=1` steps. The
  RP2040's Cortex-M0+ cores run the ARMv6-M subset the same way, faulting on what the M0+ lacks
  (unaligned accesses, Thumb-2 instructions other than BL, MSR, MRS and the barriers).
- **Security extension.** Each Cortex-M33 has Secure and Non-secure states, with their banked
  stack pointers, masks, CONTROL, SysTicks and system control registers (Secure code reaches the
  Non-secure ones through the alias at `0xe002e000`), the SAU and the RP2350's IDAU, secure
  gateways (SG), BXNS and BLXNS, the TT instructions, SecureFaults, and exceptions that cross from
  one state to the other, which stack and clear the Secure registers before a Non-secure handler
  runs. Non-secure code is always stepped one instruction at a time, as each access it makes is
  checked against the SAU and IDAU; Secure code keeps the blocks and native code.
- **ACCESSCTRL.** Its bus filters refuse, with a bus error, core and DMA accesses from a manager,
  security level or privilege an endpoint does not admit, at the datasheet's reset permissions (boot
  RAM is Secure only); its own registers want the password and a privileged writer, Non-secure code
  may pass on only what it was granted, and LOCK, CFGRESET and FORCE_CORE_NS work. GPIO_NSMASK
  decides what Non-secure code sees of the SIO GPIO registers and the IO bank and pads, including
  RP2350-E3 on an A2 in QFN-60; which GPIOs' interrupts go to IO_IRQ_BANK0_NS; and that Secure-only
  peripherals cannot be selected on Non-secure GPIOs. RESETS shows Non-secure code only its
  peripherals' resets, and DMA channels transfer at their SECCFG levels, which lock once a channel
  is programmed. `tests/trustzone` checks this and the security extension from both states.
- **Coprocessors.** The GPIO coprocessor is complete. The double-precision coprocessor (DCP), whose
  Secure and Non-secure instances are coprocessors 4 and 5, is modelled by what each canned
  operation computes rather than its internal datapath: operands are captured as doubles and the
  final read returns the IEEE result. The redundancy coprocessor (RCP, `emu/rcp.c`) follows the
  datasheet: salts, canaries, sequence counts, its checked assertions, pseudorandom delays, the NMI
  and stall an RCP fault brings on both cores, and the zeroed reads and ignored checks Non-secure
  code gets.
- **Boot ROM.** The RP2350 runs Raspberry Pi's ROM, built in. `emu/rom.c` holds a stand-in for it,
  which `--rp2350-rom stand-in` runs instead. The stand-in follows the real boot path (watchdog and
  reboot parameters, the picobin block loop, embedded and standalone partition tables, ATRANS
  address translation, load maps, BOOTSEL when nothing boots), runs core 1's launch handshake, and
  implements the ROM API the SDK and uf2loader call: flash operations, partition queries,
  `chain_image`, `reboot`, `get_sys_info` and so on. The RP2040 runs its real ROM, from a file the
  user supplies.
- **Peripherals.** Register-level models of what the PicoCalc firmware uses: SIO (FIFOs, spinlocks,
  doorbells and interpolators, in its Secure and Non-secure banks), IO bank and pads, TIMER0/1,
  watchdog, POWMAN's always-on timer, clocks/PLLs/tick generators, PL022 SPI, DesignWare I2C, PL011
  UART, PIO (all three blocks, stepped instruction by instruction on whole system clock cycles, so a
  fractional divider gives uneven ones as on the chip, except that the counting loop of
  pico-examples' PWM program, which PIO audio drivers use, skips ahead to its next edge, as does a
  jump to itself on X, Y or nothing, such as arduino-pico's cycle counter), 16-channel
  DMA, PWM, QMI direct mode with a W25Q flash (busy for its datasheet's typical erase and program
  times) and an APS6404 PSRAM, TRNG, SHA-256 accelerator, OTP chip ID, boot RAM and boot locks. On
  the RP2040 the same models sit behind its register layout (`emu/rp2040.c`), with its own clock
  tree, resets, SIO hardware divider, RTC, and the SSI and XIP cache it reaches flash through; XIP
  reads get flash data only while the SSI and the flash's continuous-read mode are set up as boot2
  leaves them.
- **Timing.** Emulated time is paced against the host clock. Buses move data at the rate the
  firmware programmed (a 10 kHz I2C poll of the keyboard takes as long as it does on the board).
- **Pin timing.** A pin's change reaches the PIO's inputs a cycle plus the pads' delays later
  (6.2 to 6.8 ns there and back with the default 4 mA slow-slew pad on a PicoCalc, less at higher
  drive or fast slew), then through the 2-cycle input synchroniser unless it is bypassed, and a
  device answers an edge as late as the real part would. A PIO driver that samples too early or
  too late therefore reads the same wrong bits here as on the board.
- **Devices.** The LCD, SD card and keyboard controller answer whatever drives their pins: a
  PL022 or the I2C block, a PIO program, or bit-banging. A PIO program that shifts one bit out per clock (the
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
- **USB.** `emu/usb.c` is the controller as a full-speed device. Its write-one-to-clear bits clear
  through the atomic aliases as well, and it reads the endpoint and buffer control words in DPRAM
  and writes them back as each transaction completes, with double buffering, interrupts per buffer
  or per two, stalls, NAKs, aborts, and suspend after 3 ms without bus activity. `emu/usbhost.c` is
  the host on the cable. Each of its transactions happens at once but is followed by the time its
  packets would take at 12 Mb/s, and one the device NAKs is retried sooner or later depending on how
  long the device has had nothing. On a CDC ACM port it keeps a bulk IN transfer waiting, as Linux's
  cdc_acm does, and sends from a 4 KB buffer that `frontend/serial.c` fills from the terminal or
  the window's `frontend/terminal.c`, a VT100 emulator the window draws cell by cell. `emu/usbmsc.c`
  is its mass storage driver: SCSI commands over the bulk-only transport, with the stalls, halt
  clearing and data toggles that transport has, a FAT reader for mounting, and 64 KB writes for a
  copy. `emu/usbip.c` is the USB/IP server: its sockets are polled from an event, as the WiFi's are,
  so it too runs on the emulation thread, with a queue of URBs for each endpoint.
- **Sound.** `emu/pinwave.c` records the waveform of the audio pins without touching the chip: a
  PWM output as its slice settings, so a fast carrier costs nothing per cycle, and any other
  driver as edges stamped with the time the core or PIO state machine made them. It integrates
  that into the fraction of each 1/192000 s the pin was high. `sim/devices/audio.c` puts that
  through the board's filters, worked out from its resistors and capacitors, and resamples it to
  48 kHz. A PWM carrier above 20 kHz is taken as its average, since it is inaudible.

## Building

You need CMake 3.16+ and a C and C++ compiler. Nothing from the Pico SDK or an ARM toolchain is
needed.

```sh
cmake -S . -B build
cmake --build build -j
```

If SDL3 is not installed the build downloads SDL 3.4.18 (hash-pinned) and links it statically. On
Linux that needs the X11 and Wayland development headers, plus the audio ones for sound; the
packages are listed in `ci/build-linux.sh`, and SDL's configure step names any X11 one that is
missing. Installing the system package (`libsdl3-dev`, `brew install sdl3`) skips the download;
`-DPICOCALC_SIM_SDL=OFF` builds a headless-only binary. The window's menus are drawn with Dear ImGui
1.92.9b, downloaded (hash-pinned) with SDL unless `IMGUI_DIR` specifies a copy; its C++ runtime is
linked in, so the binary still needs only the C library. 

The build produces `build/picocalc-sim`, the emulator, and `build/tools/mkcard/picocalc-sim-mkcard`,
which makes SD card images.

### Windows

The Windows build is cross-compiled from Linux with MinGW-w64 (`apt install mingw-w64` on Debian
and Ubuntu):

```sh
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
cmake --build build-win -j
```

That gives `build-win/picocalc-sim.exe`, `build-win/picocalc-sim-console.exe` and
`build-win/tools/mkcard/picocalc-sim-mkcard.exe`. Each is a single file that needs only DLLs Windows
ships with, so it can be copied to any 64-bit Windows 10 or 11 machine. MSYS2's MinGW-w64 should
build it natively on Windows the same way, but that is untested.

### Checking a change to the emulator

`PICOCALC_SIM_STOP_AT_MS=MS` stops a run at exactly that much emulated time, and with `--trace` the
exit report ends with a digest of both cores' registers, cycle time and instruction counts and all
of SRAM. Unthrottled headless runs are deterministic, so a change that should not alter behaviour
(such as a faster CPU path) must leave every digest as it was, and the three ways the CPU can run
(native code, `PICOCALC_SIM_NO_JIT=1` and `PICOCALC_SIM_NO_BLOCKS=1`) must give the same one:

```sh
PICOCALC_SIM_STOP_AT_MS=3000 build/picocalc-sim --headless --unthrottled --trace --flash none --no-card fw.uf2 2>&1 | grep digest
```

`PICOCALC_SIM_RESTART_AT_MS=MS` restarts the machine once at that emulated time, as the script
command `restart` does, and the run then starts again from power-on. Nothing of the first run may
survive it, so with `PICOCALC_SIM_STOP_AT_MS` the second run's digest must match a fresh launch's
(`ci/restart-test.sh` checks this for each firmware).

Instructions that the firmware runs rarely can escape both checks, so `tests/jit` runs a loop of
them often enough to be compiled and checks its result in all three ways (`ci/jit-test.sh`). It
covers REV, REV16, REVSH and CLZ, whose 32-bit encodings the JIT once compiled wrongly; an
instruction found to misbehave belongs there too.

`tests/arduino` checks two things arduino-pico firmware relies on (`ci/arduino-test.sh`): I2C to the
keyboard controller bit-banged on its pins, as arduino-pico's probe of an address is, and the PIO
cycle counter it runs under FreeRTOS, which picocalc-sim skips ahead instead of stepping, counted
against the timer. The `--trace` report says how many PIO instructions each block stepped one at a
time, and the check fails if the counter's loops were among them.

### Measuring the hardware

`tools/psramprobe` is firmware for a real PicoCalc, built on its own with the Pico SDK (the first
line of its `CMakeLists.txt` says how). Started from uf2loader, it measures in PIO cycles how long
SCK and the mainboard PSRAM's output take to reach the PIO at 96 to 252 MHz, runs rp2040-psram's
read and write programs at several clocks and dividers, and writes the results to `PSPROBE.TXT` on
the card. The emulator's pin timing comes from one such report, and running the probe in the
emulator should give the same one.

## Status

Tested with UF2s built from my own PicoCalc projects (`pcsh`, `picocalc_lua`, `picocalc-z80` and
`ftpserver`), pelrun's uf2Loader, BlairLeduc's PicoCalc shell, MicroPython, and ClockworkPi's PicoMite build:

| Firmware | Pico 2 | Pico 2 W | Pimoroni Plus 2 W | Pico | Pico W |
| --- | --- | --- | --- | --- | --- |
| pcsh | shell, SD, Lua | WiFi scan and join, DHCP, DNS, NTP, HTTP, ping to the gateway | boots with an 8 MB PSRAM heap | shell, SD | boots |
| picocalc_lua | boots, SD mounted, `sound` notes, `psram` on the mainboard PSRAM | WiFi scan and join, `psram` | boots | boots, `psram` | boots |
| picocalc-z80 | boots to its menu | boots to its menu | boots, finds and benchmarks the PSRAM | boots to its menu | boots to its menu |
| ftpserver | (no Pico 2 build) | FTP through a forward: login, list, transfers | same | (no Pico build) | same as Pico 2 W |
| uf2loader 2.4.1 | menu, flashing and chaining apps | not tried | not tried | menu, flashing and booting apps | not tried |
| PicoCalc shell 1.2 | running apps by name, `menu`, `bootsel`, its patched loader | not tried | not tried | RP2350 only | RP2350 only |
| PicoMite 6.00.02RC23 | MMBasic prompt after its first-boot flash format (about 20 s), `PLAY TONE` | not tried | same, on the non-W Plus 2 | not tried | not tried |
| MicroPython 1.29 | REPL on the USB serial console (`RPI_PICO2` build) | not tried | not tried | REPL on the USB serial console (`RPI_PICO` build) | not tried |

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
- **Mainboard PSRAM timing** was measured on my PicoCalc hardware, with the default 4 mA slow-slew pads and
  at 12 mA fast slew; other pad settings are interpolated, and quad mode is unmeasured. A read
  that lands within a fraction of a nanosecond of a change, which varies from read to read on the
  board, gives the same answer every time here.
- **USB** has the controller only in device mode (host mode keeps its registers but moves nothing),
  and the host on the cable drives only a CDC ACM serial port and a mass storage drive, the first of
  each: other interfaces are for a computer attached over USB/IP, and the drive is read only to
  mount it and written only to copy a UF2. USB/IP carries no isochronous transfers, and has been
  checked with Linux's own attach but not yet usbip-win2's. The stand-in emulated RP2350 ROM
  (`--rp2350-rom stand-in`) has no USB boot.
- **RP2040 boards** use the pin and PSRAM timing measured on my RP2350, not yet checked on an
  RP2040. A RAM image the ROM jumps to straight after reset finds XIP not yet set up, and its
  flash reads give zeros; what a real RP2040 returns there is unverified.
- **RISC-V** images for the RP2350 are not supported.
- **Windows:** `picocalc-sim-mkcard.exe` reads host file names in the system code page, so files
  whose names are not plain ASCII may not copy.
- Privilege matters only to ACCESSCTRL and TT: otherwise unprivileged code can do whatever
  privileged code can. The MPU is consulted only by TT, not by loads, stores or fetches. ACCESSCTRL
  does not filter instruction fetches, or Secure loads and stores that the cores make straight to
  SRAM, ROM or cached XIP; PIO blocks still see Secure-only GPIOs and each other, DMA channels still
  see Secure-only peripherals' DREQs, and the DMA's own MPU and the security of its registers and
  interrupts are not modelled. OTP reads as unprogrammed apart from the chip ID.
- Assumptions about the keyboard controller I could not check against hardware are marked in
  `sim/devices/southbridge.c` (hold after 300 ms, release codes).

## Roadmap

1. The RP2040 boards' pin and PSRAM timing measured on a physical Pico board.
2. A macOS build.
3. Bridged networking so the firmware gets an address on the LAN.
4. Add the option to allow auto-repeat of keys held down.

## Licence

picocalc-sim is released under the [MIT licence](LICENSE). Raspberry Pi's RP2350 boot ROMs in
[roms/rp2350](roms/rp2350) keep their own BSD licence, and files taken from other projects keep the
notices at their tops.
