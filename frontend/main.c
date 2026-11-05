#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "picocalc-sim/devices.h"
#include "picocalc-sim/frontend.h"
#include "picocalc-sim/sim.h"

static void usage(const char *argv0)
{
    printf("usage: %s [options] [FIRMWARE.uf2]\n"
           "\n"
           "Emulates a PicoCalc with an RP2350 or RP2040 board. A UF2 given on the command line is written into\n"
           "flash as if dropped on the BOOTSEL drive; without one, whatever the flash already holds\n"
           "boots, as when the PicoCalc is switched on.\n"
           "\n"
           "  -b, --board NAME       pico2, pico2_w, pimoroni_pico_plus2_w, pico or pico_w (default: from\n"
           "                         the UF2, otherwise pico2)\n"
           "      --rom FILE         the RP2040 boot ROM, as b2.elf from github.com/raspberrypi/\n"
           "                         pico-bootrom-rp2040 or a 16 KB image (default\n"
           "                         ~/.local/share/picocalc-sim/rp2040-rom.elf or .bin); pico and\n"
           "                         pico_w need it\n"
           "      --rp2350-rom FILE  run Raspberry Pi's RP2350 boot ROM instead of picocalc-sim's own:\n"
           "                         the A2 or A4 bootrom-combined.bin from github.com/raspberrypi/\n"
           "                         pico-bootrom-rp2350\n"
           "  -f, --flash FILE       flash image to use and keep (default\n"
           "                         ~/.local/share/picocalc-sim/BOARD.flash; 'none' for one kept in\n"
           "                         memory until picocalc-sim exits)\n"
           "      --erase-flash      start with erased flash\n"
           "  -c, --card IMAGE       SD card image (default ~/.local/share/picocalc-sim/sd.img if it\n"
           "                         exists)\n"
           "      --no-card          start with the card slot empty\n"
           "      --headless         no window; use with --script and screenshots\n"
           "  -s, --script FILE      drive the keyboard from FILE (see frontend/script.c)\n"
           "  -t, --timeout SECONDS  quit after this long\n"
           "      --scale N          window scale factor (default 2)\n"
           "      --fast             run SPI, I2C and UART transfers instantly\n"
           "      --unthrottled      run as fast as the host allows instead of at device speed\n"
           "      --console          print what the firmware sends on UART0\n"
           "      --battery PCT      battery level the keyboard controller reports (default 85)\n"
           "      --charging         report the battery as charging\n"
           "      --screenshot-dir D where F11 saves screenshots (default .)\n"
           "      --volume PCT       loudness of the sound in the window (default 50)\n"
           "      --mute             no sound in the window\n"
           "      --wav FILE         record the sound to FILE (48 kHz stereo WAV) in emulated time\n"
           "      --wifi-ap SSID[:PASSWORD]\n"
           "                         a virtual access point for scans and joins; repeat for more. Without\n"
           "                         one, any network name and password the firmware tries will join\n"
           "      --forward [ADDR:]HOSTPORT:PORT\n"
           "                         let connections to HOSTPORT on the host (127.0.0.1 unless ADDR is\n"
           "                         given; * for all interfaces) reach the firmware's TCP PORT; repeatable\n"
           "      --no-wifi          leave the W boards' radio out, as if it never answered\n"
           "      --trace            log faults, unknown ROM calls and bus errors\n"
           "\n"
           "In the window, the menus change these settings and restart the machine. F11 saves a\n"
           "screenshot, F12 is the reset button, Shift+F12 resets into BOOTSEL, and dropping a UF2 file on\n"
           "the window flashes it (a .img file goes in as the SD card). PgUp, PgDn, Home, End, Insert and\n"
           "Pause are sent as the Shift chords the PicoCalc keyboard uses. What the window changes is\n"
           "saved in settings.ini in the data directory for its next launch, where an option given here\n"
           "overrides it; headless runs ignore that file.\n",
           argv0);
}

int picocalc_sim_frontend_headless_run(void)
{
    if (!picocalc_sim_machine_running())
        return 1;
    uint64_t start = picocalc_sim_mono_ns();
    int code = 0;
    for (;;) {
        if (picocalc_sim_restart_requested() && !picocalc_sim_machine_restart())
            return 1;
        southbridge_tick(picocalc_sim_picocalc.sb);
        if (picocalc_sim_quit_requested(&code))
            return code;
        if (picocalc_sim_opts.timeout_s > 0 && (picocalc_sim_mono_ns() - start) / 1e9 >= picocalc_sim_opts.timeout_s)
            return 0;
        picocalc_sim_sleep_ns(10000000ull);
    }
}

static const char *default_card(void)
{
    static char path[4096];
    picocalc_sim_data_path(path, sizeof(path), "sd.img", false);
    struct stat st;
    return stat(path, &st) == 0 ? path : NULL;
}

int main(int argc, char **argv)
{
    const char *card = NULL;
    bool no_card = false;

    enum {
        OPT_HEADLESS = 256,
        OPT_SCALE,
        OPT_FAST,
        OPT_UNTHROTTLED,
        OPT_CONSOLE,
        OPT_BATTERY,
        OPT_CHARGING,
        OPT_SHOTS,
        OPT_NOCARD,
        OPT_TRACE,
        OPT_ERASE,
        OPT_WIFI_AP,
        OPT_FORWARD,
        OPT_NO_WIFI,
        OPT_VOLUME,
        OPT_MUTE,
        OPT_WAV,
        OPT_ROM,
        OPT_RP2350_ROM,
    };
    static const struct option longopts[] = {
        {"board", required_argument, NULL, 'b'},
        {"flash", required_argument, NULL, 'f'},
        {"rom", required_argument, NULL, OPT_ROM},
        {"rp2350-rom", required_argument, NULL, OPT_RP2350_ROM},
        {"erase-flash", no_argument, NULL, OPT_ERASE},
        {"card", required_argument, NULL, 'c'},
        {"no-card", no_argument, NULL, OPT_NOCARD},
        {"headless", no_argument, NULL, OPT_HEADLESS},
        {"script", required_argument, NULL, 's'},
        {"timeout", required_argument, NULL, 't'},
        {"scale", required_argument, NULL, OPT_SCALE},
        {"fast", no_argument, NULL, OPT_FAST},
        {"unthrottled", no_argument, NULL, OPT_UNTHROTTLED},
        {"console", no_argument, NULL, OPT_CONSOLE},
        {"battery", required_argument, NULL, OPT_BATTERY},
        {"charging", no_argument, NULL, OPT_CHARGING},
        {"screenshot-dir", required_argument, NULL, OPT_SHOTS},
        {"trace", no_argument, NULL, OPT_TRACE},
        {"wifi-ap", required_argument, NULL, OPT_WIFI_AP},
        {"forward", required_argument, NULL, OPT_FORWARD},
        {"no-wifi", no_argument, NULL, OPT_NO_WIFI},
        {"volume", required_argument, NULL, OPT_VOLUME},
        {"mute", no_argument, NULL, OPT_MUTE},
        {"wav", required_argument, NULL, OPT_WAV},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    // The long names of the options given, which the settings file then leaves alone.
    const char *given[sizeof(longopts) / sizeof(longopts[0])];
    int ngiven = 0;
    int opt;
    while ((opt = getopt_long(argc, argv, "b:f:c:s:t:h", longopts, NULL)) != -1) {
        for (const struct option *o = longopts; o->name; o++)
            if (o->val == opt && ngiven < (int)(sizeof(given) / sizeof(given[0])))
                given[ngiven++] = o->name;
        switch (opt) {
        case 'b': picocalc_sim_opts.board = optarg; break;
        case 'f': picocalc_sim_opts.flash_path = optarg; break;
        case OPT_ROM: picocalc_sim_opts.rom_path = optarg; break;
        case OPT_RP2350_ROM: picocalc_sim_opts.rp2350_rom_path = optarg; break;
        case OPT_ERASE: picocalc_sim_opts.erase_flash = true; break;
        case 'c': card = optarg; break;
        case OPT_NOCARD: no_card = true; break;
        case OPT_HEADLESS: picocalc_sim_opts.headless = true; break;
        case 's': picocalc_sim_opts.script_path = optarg; break;
        case 't': picocalc_sim_opts.timeout_s = atof(optarg); break;
        case OPT_SCALE: picocalc_sim_opts.scale = atoi(optarg) > 0 ? atoi(optarg) : 1; break;
        case OPT_FAST: picocalc_sim_opts.fast = true; break;
        case OPT_UNTHROTTLED: picocalc_sim_opts.unthrottled = true; break;
        case OPT_CONSOLE: picocalc_sim_opts.console = true; break;
        case OPT_BATTERY: picocalc_sim_opts.battery = atoi(optarg); break;
        case OPT_CHARGING: picocalc_sim_opts.charging = true; break;
        case OPT_SHOTS: picocalc_sim_opts.screenshot_dir = optarg; break;
        case OPT_TRACE: picocalc_sim_opts.trace = true; break;
        case OPT_WIFI_AP:
            if (picocalc_sim_opts.wifi_ap_count < 16)
                picocalc_sim_opts.wifi_aps[picocalc_sim_opts.wifi_ap_count++] = optarg;
            break;
        case OPT_FORWARD:
            if (picocalc_sim_opts.forward_count < 16)
                picocalc_sim_opts.forwards[picocalc_sim_opts.forward_count++] = optarg;
            break;
        case OPT_NO_WIFI: picocalc_sim_opts.no_wifi = true; break;
        case OPT_VOLUME: {
            int v = atoi(optarg);
            picocalc_sim_opts.volume = v < 0 ? 0 : v > 100 ? 100 : v;
            break;
        }
        case OPT_MUTE: picocalc_sim_opts.mute = true; break;
        case OPT_WAV: picocalc_sim_opts.wav_path = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }
    if (optind < argc)
        picocalc_sim_opts.uf2_path = argv[optind++];
    if (optind < argc) {
        usage(argv[0]);
        return 2;
    }
    bool window = !picocalc_sim_opts.headless && picocalc_sim_frontend_sdl_available();
    if (window) {
        picocalc_sim_settings_load(given, ngiven);
        const char *saved = picocalc_sim_settings_get("card");
        if (saved && *saved && !card && !no_card) {
            if (!strcmp(saved, "none"))
                no_card = true;
            else
                card = saved;
        }
    }
    if (!card && !no_card)
        card = default_card();
    picocalc_sim_opts.card_path = no_card ? NULL : card;

    // The window can show why the machine did not start, and has the settings to fix it.
    if (!picocalc_sim_machine_start() && !window)
        return 1;
    if (!card && !no_card)
        picocalc_sim_log("no SD card (make one with picocalc-sim-mkcard and pass it with --card)");

    if (picocalc_sim_opts.script_path)
        picocalc_sim_script_start(picocalc_sim_opts.script_path);

    int code;
    if (window)
        code = picocalc_sim_frontend_sdl_run();
    else
        code = picocalc_sim_frontend_headless_run();

    picocalc_sim_machine_stop();
    fflush(NULL);
    exit(code);
}
