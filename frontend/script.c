#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "picocalc-sim/frontend.h"
#include "picocalc-sim/sim.h"

// Drives the keyboard from a file, one command per line:
//   wait <ms>            type <text>          key <chord>
//   screenshot <file>    restart              quit [code]          # comment
//   usb on|off           (plugs the board's USB port into a host, or unplugs it)
//   usb send <text>      (sends text to the firmware's USB serial port, which --usb-console opens)
//   usb copy <file>      (copies a UF2 onto the boot ROM's drive once it is mounted)
// type and usb send take the rest of the line with C escapes (\n \r \t \e \b \\); key takes
// chords such as ctrl+c, shift+up, brk or f6.

#define KEY_HOLD_MS 30
#define KEY_GAP_MS 40

static char *script_path;

// Undoes the C escapes in s, in place, giving the length.
static size_t unescape(char *s)
{
    char *o = s;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'e': c = 0x1b; break;
            case 'b': c = '\b'; break;
            case '\\': c = '\\'; break;
            default: c = *p; break;
            }
        }
        *o++ = c;
    }
    return (size_t)(o - s);
}

static void unescape_and_type(char *s)
{
    size_t n = unescape(s);
    for (size_t i = 0; i < n; i++)
        if (!picocalc_sim_keys_type_char(s[i], KEY_HOLD_MS, KEY_GAP_MS))
            picocalc_sim_log("script: the PicoCalc has no key for 0x%02x", (unsigned char)s[i]);
}

static void usb_send(int lineno, char *s)
{
    size_t n = unescape(s), off = 0;
    while (off < n && !picocalc_sim_quit_requested(NULL)) {
        size_t k = picocalc_sim_usb_serial_send((const uint8_t *)s + off, n - off);
        if (!k && !picocalc_sim_usb_serial_is_open()) {
            picocalc_sim_log("script line %d: the USB serial port is not open", lineno);
            return;
        }
        off += k;
        if (!k)
            picocalc_sim_sleep_ns(10000000ull);
    }
}

static void *script_thread(void *arg)
{
    (void)arg;
    FILE *f = fopen(script_path, "r");
    if (!f) {
        picocalc_sim_log("cannot open script %s", script_path);
        picocalc_sim_request_quit(2);
        return NULL;
    }
    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        line[strcspn(line, "\r\n")] = '\0';
        char *cmd = line;
        while (isspace((unsigned char)*cmd))
            cmd++;
        if (!*cmd || *cmd == '#')
            continue;
        char *rest = cmd;
        while (*rest && !isspace((unsigned char)*rest))
            rest++;
        if (*rest)
            *rest++ = '\0';
        if (strcmp(cmd, "type") != 0)
            while (isspace((unsigned char)*rest))
                rest++;

        if (!strcmp(cmd, "wait")) {
            picocalc_sim_sleep_ns((uint64_t)strtoul(rest, NULL, 10) * 1000000ull);
        } else if (!strcmp(cmd, "type")) {
            unescape_and_type(rest);
        } else if (!strcmp(cmd, "key")) {
            if (!picocalc_sim_keys_chord(rest, KEY_HOLD_MS))
                picocalc_sim_log("script line %d: unknown key '%s'", lineno, rest);
            picocalc_sim_sleep_ns(KEY_GAP_MS * 1000000ull);
        } else if (!strcmp(cmd, "screenshot")) {
            picocalc_sim_screenshot(rest, NULL, 0);
        } else if (!strcmp(cmd, "restart")) {
            unsigned n = picocalc_sim_machine_starts();
            picocalc_sim_request_restart();
            while (picocalc_sim_machine_starts() == n && !picocalc_sim_quit_requested(NULL))
                picocalc_sim_sleep_ns(10000000ull);
        } else if (!strcmp(cmd, "usb") && (!strcmp(rest, "on") || !strcmp(rest, "off"))) {
            picocalc_sim_opts.usb = !strcmp(rest, "on");
            picocalc_sim_usb_plug(picocalc_sim_opts.usb);
        } else if (!strcmp(cmd, "usb") && !strncmp(rest, "send ", 5)) {
            usb_send(lineno, rest + 5);
        } else if (!strcmp(cmd, "usb") && !strncmp(rest, "copy ", 5)) {
            char err[256];
            if (!picocalc_sim_usb_drive_copy(rest + 5, err, sizeof(err)))
                picocalc_sim_log("script line %d: %s", lineno, err);
        } else if (!strcmp(cmd, "quit")) {
            fclose(f);
            picocalc_sim_request_quit(*rest ? atoi(rest) : 0);
            return NULL;
        } else {
            picocalc_sim_log("script line %d: unknown command '%s'", lineno, cmd);
        }
    }
    fclose(f);
    return NULL;
}

void picocalc_sim_script_start(const char *path)
{
    script_path = strdup(path);
    pthread_t t;
    pthread_create(&t, NULL, script_thread, NULL);
    pthread_detach(t);
}
