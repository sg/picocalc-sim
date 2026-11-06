#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "picocalc-sim/bus.h"
#include "picocalc-sim/devices.h"
#include "picocalc-sim/frontend.h"
#include "picocalc-sim/keys.h"
#include "picocalc-sim/sim.h"

static void sleep_ms(unsigned ms)
{
    picocalc_sim_sleep_ns((uint64_t)ms * 1000000ull);
}

// A restart frees the devices and makes new ones with the machine locked, and a script's keys
// can arrive in between.
static void send_key(uint8_t key, bool pressed)
{
    picocalc_sim_machine_lock();
    if (picocalc_sim_picocalc.sb)
        southbridge_key(picocalc_sim_picocalc.sb, key, pressed);
    picocalc_sim_machine_unlock();
}

void picocalc_sim_keys_press(uint8_t key)
{
    // The power button is the only way back from a power-off, and any key stands in for it.
    if (picocalc_sim_state() == PICOCALC_SIM_POWERED_OFF) {
        picocalc_sim_power_button();
        return;
    }
    send_key(key, true);
}

void picocalc_sim_keys_release(uint8_t key)
{
    send_key(key, false);
}

static const char shifted_from[] = "1234567890-=[]\\;',./`";
static const char shifted_to[] = "!@#$%^&*()_+{}|:\"<>?~";

// The physical key that produces c, and whether Shift has to be held for it.
static bool key_for_char(char c, uint8_t *key, bool *shift)
{
    *shift = false;
    if (c >= 'a' && c <= 'z') {
        *key = (uint8_t)c;
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        *key = (uint8_t)tolower((unsigned char)c);
        *shift = true;
        return true;
    }
    const char *p = strchr(shifted_to, c);
    if (p && c) {
        *key = (uint8_t)shifted_from[p - shifted_to];
        *shift = true;
        return true;
    }
    if (c && strchr(shifted_from, c)) {
        *key = (uint8_t)c;
        return true;
    }
    switch (c) {
    case ' ':
        *key = PC_KEY_SPACE;
        return true;
    case '\n':
    case '\r':
        *key = PC_KEY_ENTER;
        return true;
    case '\t':
        *key = PC_KEY_TAB;
        return true;
    case '\b':
        *key = PC_KEY_BACKSPACE;
        return true;
    case 0x1b:
        *key = PC_KEY_ESC;
        return true;
    default:
        return false;
    }
}

bool picocalc_sim_keys_type_char(char c, unsigned hold_ms, unsigned gap_ms)
{
    uint8_t key;
    bool shift;
    if (!key_for_char(c, &key, &shift))
        return false;
    // Caps Lock flips letters, so typing a given case may need Shift the other way.
    picocalc_sim_machine_lock();
    bool caps = picocalc_sim_picocalc.sb && southbridge_caps_lock(picocalc_sim_picocalc.sb);
    picocalc_sim_machine_unlock();
    if (key >= 'a' && key <= 'z' && caps)
        shift = !shift;
    if (shift)
        picocalc_sim_keys_press(PC_KEY_LSHIFT);
    picocalc_sim_keys_press(key);
    sleep_ms(hold_ms);
    picocalc_sim_keys_release(key);
    if (shift)
        picocalc_sim_keys_release(PC_KEY_LSHIFT);
    sleep_ms(gap_ms);
    return true;
}

typedef struct {
    const char *name;
    uint8_t key;
    bool shift;
} named_key_t;

// Names for keys a chord can end in; the shifted ones are what the PicoCalc itself requires.
static const named_key_t named_keys[] = {
    {"enter", PC_KEY_ENTER, false},     {"return", PC_KEY_ENTER, false},  {"esc", PC_KEY_ESC, false},
    {"escape", PC_KEY_ESC, false},      {"tab", PC_KEY_TAB, false},       {"bs", PC_KEY_BACKSPACE, false},
    {"backspace", PC_KEY_BACKSPACE, false}, {"del", PC_KEY_DEL, false},  {"delete", PC_KEY_DEL, false},
    {"up", PC_KEY_UP, false},           {"down", PC_KEY_DOWN, false},     {"left", PC_KEY_LEFT, false},
    {"right", PC_KEY_RIGHT, false},     {"space", PC_KEY_SPACE, false},   {"caps", PC_KEY_CAPS_LOCK, false},
    {"capslock", PC_KEY_CAPS_LOCK, false}, {"f1", PC_KEY_F1, false},     {"f2", PC_KEY_F2, false},
    {"f3", PC_KEY_F3, false},           {"f4", PC_KEY_F4, false},         {"f5", PC_KEY_F5, false},
    {"f6", PC_KEY_F1, true},            {"f7", PC_KEY_F2, true},          {"f8", PC_KEY_F3, true},
    {"f9", PC_KEY_F4, true},            {"f10", PC_KEY_F5, true},         {"home", PC_KEY_TAB, true},
    {"end", PC_KEY_DEL, true},          {"pgup", PC_KEY_UP, true},        {"pageup", PC_KEY_UP, true},
    {"pgdn", PC_KEY_DOWN, true},        {"pagedown", PC_KEY_DOWN, true},  {"ins", PC_KEY_ENTER, true},
    {"insert", PC_KEY_ENTER, true},     {"brk", PC_KEY_ESC, true},        {"break", PC_KEY_ESC, true},
};

bool picocalc_sim_keys_chord(const char *spec, unsigned hold_ms)
{
    uint8_t mods[4];
    int nmods = 0;
    uint8_t key = 0;
    bool need_shift = false;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", spec);

    char *save = NULL;
    for (char *tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        if (!strcasecmp(tok, "ctrl") && nmods < 4) {
            mods[nmods++] = PC_KEY_CTRL;
        } else if (!strcasecmp(tok, "alt") && nmods < 4) {
            mods[nmods++] = PC_KEY_ALT;
        } else if ((!strcasecmp(tok, "shift") || !strcasecmp(tok, "lshift")) && nmods < 4) {
            mods[nmods++] = PC_KEY_LSHIFT;
        } else if (!strcasecmp(tok, "rshift") && nmods < 4) {
            mods[nmods++] = PC_KEY_RSHIFT;
        } else {
            bool found = false;
            for (size_t i = 0; i < sizeof(named_keys) / sizeof(named_keys[0]); i++) {
                if (!strcasecmp(tok, named_keys[i].name)) {
                    key = named_keys[i].key;
                    need_shift = named_keys[i].shift;
                    found = true;
                    break;
                }
            }
            if (!found && strlen(tok) == 1 && !key_for_char(tok[0], &key, &need_shift))
                return false;
            if (!found && strlen(tok) != 1)
                return false;
        }
    }
    if (!key)
        return false;

    bool shift_in_mods = false;
    for (int i = 0; i < nmods; i++)
        shift_in_mods |= mods[i] == PC_KEY_LSHIFT || mods[i] == PC_KEY_RSHIFT;
    if (need_shift && !shift_in_mods && nmods < 4)
        mods[nmods++] = PC_KEY_LSHIFT;

    for (int i = 0; i < nmods; i++)
        picocalc_sim_keys_press(mods[i]);
    picocalc_sim_keys_press(key);
    sleep_ms(hold_ms);
    picocalc_sim_keys_release(key);
    for (int i = nmods - 1; i >= 0; i--)
        picocalc_sim_keys_release(mods[i]);
    return true;
}

bool picocalc_sim_screenshot(const char *path, char *why, size_t whylen)
{
    static uint32_t pixels[PICOCALC_SIM_LCD_WIDTH * PICOCALC_SIM_LCD_HEIGHT];
    char reason[256];
    picocalc_sim_machine_lock();
    bool have = picocalc_sim_picocalc.lcd != NULL;
    if (have)
        st7365p_render(picocalc_sim_picocalc.lcd, pixels, southbridge_lcd_backlight(picocalc_sim_picocalc.sb));
    picocalc_sim_machine_unlock();
    errno = 0;
    if (!have) {
        snprintf(reason, sizeof(reason), "the machine is stopped");
        picocalc_sim_log("no screenshot while the machine is stopped");
    } else if (!picocalc_sim_png_write(path, pixels, PICOCALC_SIM_LCD_WIDTH, PICOCALC_SIM_LCD_HEIGHT)) {
        // Creating a file fails with ENOENT only when its folder is missing.
        snprintf(reason, sizeof(reason), "%s",
                 errno == ENOENT ? "the folder does not exist"
                 : errno         ? strerror(errno)
                                 : "the file could not be written");
        picocalc_sim_log("cannot write %s: %s", path, reason);
    } else {
        picocalc_sim_log("screenshot: %s", path);
        return true;
    }
    if (why)
        snprintf(why, whylen, "%s", reason);
    return false;
}

bool picocalc_sim_screenshot_auto(char *path, size_t pathlen, char *why, size_t whylen)
{
    static int counter;
    const char *dir = picocalc_sim_opts.screenshot_dir && *picocalc_sim_opts.screenshot_dir
                          ? picocalc_sim_opts.screenshot_dir
                          : ".";
    // The full path, so that it can be found again whatever folder picocalc-sim was started in.
    char full[1024];
#ifdef _WIN32
    const char *sep = "\\";
    if (_fullpath(full, dir, sizeof(full)))
        dir = full;
#else
    const char *sep = "/";
    char *real = realpath(dir, NULL), cwd[768];
    if (real) {
        snprintf(full, sizeof(full), "%s", real);
        free(real);
        dir = full;
    } else if (dir[0] != '/' && getcwd(cwd, sizeof(cwd))) {
        // realpath needs the folder to exist; one that does not still gets a full path to report.
        snprintf(full, sizeof(full), "%s/%s", strcmp(cwd, "/") ? cwd : "", dir);
        dir = full;
    }
#endif
    size_t n = strlen(dir);
    if (n > 0 && (dir[n - 1] == '/' || dir[n - 1] == sep[0]))
        sep = "";
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    snprintf(path, pathlen, "%s%spicocalc-%s-%d.png", dir, sep, stamp, ++counter);
    return picocalc_sim_screenshot(path, why, whylen);
}

const char *picocalc_sim_window_title(void)
{
    static char title[256];
    if (!picocalc_sim_machine_running())
        return "PicoCalc - not running";
    const char *state = "";
    switch (picocalc_sim_state()) {
    case PICOCALC_SIM_RUNNING:
        break;
    case PICOCALC_SIM_HALTED:
        state = " - HALTED (F12 resets)";
        break;
    case PICOCALC_SIM_BOOTSEL:
        state = " - BOOTSEL: drop a UF2 on the window (F12 resets)";
        break;
    case PICOCALC_SIM_POWERED_OFF:
        state = " - POWERED OFF (any key)";
        break;
    }
    double speed = picocalc_sim_speed();
    char speed_note[32] = "";
    if (picocalc_sim_state() == PICOCALC_SIM_RUNNING && speed < 0.95)
        snprintf(speed_note, sizeof(speed_note), " - %.0f%% speed", speed * 100);
    snprintf(title, sizeof(title), "PicoCalc [%s]%s%s%s", picocalc_sim_board_name(),
             southbridge_caps_lock(picocalc_sim_picocalc.sb) ? " - CAPS" : "", speed_note, state);
    return title;
}
