#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pcsim/devices.h"
#include "pcsim/frontend.h"
#include "pcsim/sim.h"

static void usage(const char *argv0)
{
    printf("usage: %s [options] [FIRMWARE.uf2]\n"
           "\n"
           "Emulates a PicoCalc with an RP2350 board. A UF2 given on the command line is written into\n"
           "flash as if dropped on the BOOTSEL drive; without one, whatever the flash already holds\n"
           "boots, as when the PicoCalc is switched on.\n"
           "\n"
           "  -b, --board NAME       pico2, pico2_w or pimoroni_pico_plus2_w (default: from the UF2,\n"
           "                         otherwise pico2)\n"
           "  -f, --flash FILE       flash image to use and keep (default ~/.local/share/pcsim/BOARD.flash;\n"
           "                         'none' for a flash that is discarded on exit)\n"
           "      --erase-flash      start with erased flash\n"
           "  -c, --card IMAGE       SD card image (default ~/.local/share/pcsim/sd.img if it exists)\n"
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
           "      --wifi-ap SSID[:PASSWORD]\n"
           "                         a virtual access point for scans and joins; repeat for more. Without\n"
           "                         one, any network name and password the firmware tries will join\n"
           "      --forward [ADDR:]HOSTPORT:PORT\n"
           "                         let connections to HOSTPORT on the host (127.0.0.1 unless ADDR is\n"
           "                         given; * for all interfaces) reach the firmware's TCP PORT; repeatable\n"
           "      --no-wifi          leave the W boards' radio out, as if it never answered\n"
           "      --trace            log faults, unknown ROM calls and bus errors\n"
           "\n"
           "In the window: F11 saves a screenshot, F12 is the reset button, Shift+F12 resets into BOOTSEL,\n"
           "and dropping a UF2 file on the window flashes it. PgUp, PgDn, Home, End, Insert and Pause are\n"
           "sent as the Shift chords the PicoCalc keyboard uses.\n",
           argv0);
}

int pcsim_frontend_headless_run(void)
{
    uint64_t start = pcsim_mono_ns();
    int code = 0;
    for (;;) {
        southbridge_tick(pcsim_picocalc.sb);
        if (pcsim_quit_requested(&code))
            return code;
        if (pcsim_opts.timeout_s > 0 && (pcsim_mono_ns() - start) / 1e9 >= pcsim_opts.timeout_s)
            return 0;
        pcsim_sleep_ns(10000000ull);
    }
}

static const char *default_card(void)
{
    static char path[4096];
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(path, sizeof(path), "%s/pcsim/sd.img", xdg);
    else
        snprintf(path, sizeof(path), "%s/.local/share/pcsim/sd.img", home ? home : ".");
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
    };
    static const struct option longopts[] = {
        {"board", required_argument, NULL, 'b'},
        {"flash", required_argument, NULL, 'f'},
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
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "b:f:c:s:t:h", longopts, NULL)) != -1) {
        switch (opt) {
        case 'b': pcsim_opts.board = optarg; break;
        case 'f': pcsim_opts.flash_path = optarg; break;
        case OPT_ERASE: pcsim_opts.erase_flash = true; break;
        case 'c': card = optarg; break;
        case OPT_NOCARD: no_card = true; break;
        case OPT_HEADLESS: pcsim_opts.headless = true; break;
        case 's': pcsim_opts.script_path = optarg; break;
        case 't': pcsim_opts.timeout_s = atof(optarg); break;
        case OPT_SCALE: pcsim_opts.scale = atoi(optarg) > 0 ? atoi(optarg) : 1; break;
        case OPT_FAST: pcsim_opts.fast = true; break;
        case OPT_UNTHROTTLED: pcsim_opts.unthrottled = true; break;
        case OPT_CONSOLE: pcsim_opts.console = true; break;
        case OPT_BATTERY: pcsim_opts.battery = atoi(optarg); break;
        case OPT_CHARGING: pcsim_opts.charging = true; break;
        case OPT_SHOTS: pcsim_opts.screenshot_dir = optarg; break;
        case OPT_TRACE: pcsim_opts.trace = true; break;
        case OPT_WIFI_AP:
            if (pcsim_opts.wifi_ap_count < 16)
                pcsim_opts.wifi_aps[pcsim_opts.wifi_ap_count++] = optarg;
            break;
        case OPT_FORWARD:
            if (pcsim_opts.forward_count < 16)
                pcsim_opts.forwards[pcsim_opts.forward_count++] = optarg;
            break;
        case OPT_NO_WIFI: pcsim_opts.no_wifi = true; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }
    if (optind < argc)
        pcsim_opts.uf2_path = argv[optind++];
    if (optind < argc) {
        usage(argv[0]);
        return 2;
    }
    if (!card && !no_card)
        card = default_card();

    if (!pcsim_machine_start())
        return 1;
    if (!no_card && card && !sdcard_insert(pcsim_picocalc.sd, card))
        pcsim_log("continuing with the card slot empty");
    if (!card && !no_card)
        pcsim_log("no SD card (make one with pcsim-mkcard and pass it with --card)");

    if (pcsim_opts.script_path)
        pcsim_script_start(pcsim_opts.script_path);

    int code;
    if (pcsim_opts.headless || !pcsim_frontend_sdl_available())
        code = pcsim_frontend_headless_run();
    else
        code = pcsim_frontend_sdl_run();

    pcsim_machine_stop();
    pcsim_flush_devices();
    fflush(NULL);
    exit(code);
}
