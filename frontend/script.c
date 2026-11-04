#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pcsim/frontend.h"
#include "pcsim/sim.h"

// Drives the keyboard from a file, one command per line:
//   wait <ms>            type <text>          key <chord>
//   screenshot <file>    restart              quit [code]          # comment
// type takes the rest of the line with C escapes (\n \t \e \b \\); key takes chords such as
// ctrl+c, shift+up, brk or f6.

#define KEY_HOLD_MS 30
#define KEY_GAP_MS 40

static char *script_path;

static void unescape_and_type(const char *s)
{
    for (; *s; s++) {
        char c = *s;
        if (c == '\\' && s[1]) {
            s++;
            switch (*s) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'e': c = 0x1b; break;
            case 'b': c = '\b'; break;
            case '\\': c = '\\'; break;
            default: c = *s; break;
            }
        }
        if (!pcsim_keys_type_char(c, KEY_HOLD_MS, KEY_GAP_MS))
            pcsim_log("script: the PicoCalc has no key for 0x%02x", (unsigned char)c);
    }
}

static void *script_thread(void *arg)
{
    (void)arg;
    FILE *f = fopen(script_path, "r");
    if (!f) {
        pcsim_log("cannot open script %s", script_path);
        pcsim_request_quit(2);
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
            pcsim_sleep_ns((uint64_t)strtoul(rest, NULL, 10) * 1000000ull);
        } else if (!strcmp(cmd, "type")) {
            unescape_and_type(rest);
        } else if (!strcmp(cmd, "key")) {
            if (!pcsim_keys_chord(rest, KEY_HOLD_MS))
                pcsim_log("script line %d: unknown key '%s'", lineno, rest);
            pcsim_sleep_ns(KEY_GAP_MS * 1000000ull);
        } else if (!strcmp(cmd, "screenshot")) {
            pcsim_screenshot(rest);
        } else if (!strcmp(cmd, "restart")) {
            unsigned n = pcsim_machine_starts();
            pcsim_request_restart();
            while (pcsim_machine_starts() == n && !pcsim_quit_requested(NULL))
                pcsim_sleep_ns(10000000ull);
        } else if (!strcmp(cmd, "quit")) {
            fclose(f);
            pcsim_request_quit(*rest ? atoi(rest) : 0);
            return NULL;
        } else {
            pcsim_log("script line %d: unknown command '%s'", lineno, cmd);
        }
    }
    fclose(f);
    return NULL;
}

void pcsim_script_start(const char *path)
{
    script_path = strdup(path);
    pthread_t t;
    pthread_create(&t, NULL, script_thread, NULL);
    pthread_detach(t);
}
