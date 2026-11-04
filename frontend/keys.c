#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "pcsim/bus.h"
#include "pcsim/devices.h"
#include "pcsim/frontend.h"
#include "pcsim/keys.h"
#include "pcsim/sim.h"

static void sleep_ms(unsigned ms)
{
    pcsim_sleep_ns((uint64_t)ms * 1000000ull);
}

// A restart frees the devices and makes new ones with the machine locked, and a script's keys
// can arrive in between.
static void send_key(uint8_t key, bool pressed)
{
    pcsim_machine_lock();
    if (pcsim_picocalc.sb)
        southbridge_key(pcsim_picocalc.sb, key, pressed);
    pcsim_machine_unlock();
}

void pcsim_keys_press(uint8_t key)
{
    // The power button is the only way back from a power-off, and any key stands in for it.
    if (pcsim_state() == PCSIM_POWERED_OFF) {
        pcsim_power_button();
        return;
    }
    send_key(key, true);
}

void pcsim_keys_release(uint8_t key)
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

bool pcsim_keys_type_char(char c, unsigned hold_ms, unsigned gap_ms)
{
    uint8_t key;
    bool shift;
    if (!key_for_char(c, &key, &shift))
        return false;
    // Caps Lock flips letters, so typing a given case may need Shift the other way.
    pcsim_machine_lock();
    bool caps = pcsim_picocalc.sb && southbridge_caps_lock(pcsim_picocalc.sb);
    pcsim_machine_unlock();
    if (key >= 'a' && key <= 'z' && caps)
        shift = !shift;
    if (shift)
        pcsim_keys_press(PC_KEY_LSHIFT);
    pcsim_keys_press(key);
    sleep_ms(hold_ms);
    pcsim_keys_release(key);
    if (shift)
        pcsim_keys_release(PC_KEY_LSHIFT);
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

bool pcsim_keys_chord(const char *spec, unsigned hold_ms)
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
        pcsim_keys_press(mods[i]);
    pcsim_keys_press(key);
    sleep_ms(hold_ms);
    pcsim_keys_release(key);
    for (int i = nmods - 1; i >= 0; i--)
        pcsim_keys_release(mods[i]);
    return true;
}

bool pcsim_screenshot(const char *path)
{
    static uint32_t pixels[PCSIM_LCD_WIDTH * PCSIM_LCD_HEIGHT];
    pcsim_machine_lock();
    bool have = pcsim_picocalc.lcd != NULL;
    if (have)
        st7365p_render(pcsim_picocalc.lcd, pixels, southbridge_lcd_backlight(pcsim_picocalc.sb));
    pcsim_machine_unlock();
    if (!have) {
        pcsim_log("no screenshot while the machine is stopped");
        return false;
    }
    if (!pcsim_png_write(path, pixels, PCSIM_LCD_WIDTH, PCSIM_LCD_HEIGHT)) {
        pcsim_log("cannot write %s", path);
        return false;
    }
    pcsim_log("screenshot: %s", path);
    return true;
}

const char *pcsim_screenshot_auto(void)
{
    static char path[1024];
    static int counter;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    snprintf(path, sizeof(path), "%s/picocalc-%s-%d.png", pcsim_opts.screenshot_dir, stamp, ++counter);
    return pcsim_screenshot(path) ? path : NULL;
}

const char *pcsim_window_title(void)
{
    static char title[256];
    const char *state = "";
    switch (pcsim_state()) {
    case PCSIM_RUNNING:
        break;
    case PCSIM_HALTED:
        state = " - HALTED (F12 resets)";
        break;
    case PCSIM_BOOTSEL:
        state = " - BOOTSEL: drop a UF2 on the window (F12 resets)";
        break;
    case PCSIM_POWERED_OFF:
        state = " - POWERED OFF (any key)";
        break;
    }
    double speed = pcsim_speed();
    char speed_note[32] = "";
    if (pcsim_state() == PCSIM_RUNNING && speed < 0.95)
        snprintf(speed_note, sizeof(speed_note), " - %.0f%% speed", speed * 100);
    snprintf(title, sizeof(title), "PicoCalc [%s]%s%s%s", pcsim_board_name(),
             southbridge_caps_lock(pcsim_picocalc.sb) ? " - CAPS" : "", speed_note, state);
    return title;
}
