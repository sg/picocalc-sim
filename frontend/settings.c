#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "pcsim/frontend.h"
#include "pcsim/sim.h"

// The window's settings file: KEY = VALUE lines named after the long options, so it reads like a
// command line and can be edited by hand.

typedef struct {
    char *key, *value;
} entry_t;

static entry_t *entries;
static int count, cap;
static char path[4096];

// The order the file is written in; keys it does not know come after, kept for a newer pcsim.
static const char *const known[] = {
    "board",   "rom",     "flash",   "card",     "fast",   "unthrottled", "trace", "console",
    "no-wifi", "wifi-ap", "forward", "battery",  "charging", "volume",    "mute",  "scale",
    "screenshot-dir",
};

static bool is_known(const char *key)
{
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (!strcmp(key, known[i]))
            return true;
    return false;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return s;
}

static void add(const char *key, const char *value)
{
    if (count == cap) {
        cap = cap ? cap * 2 : 32;
        entries = realloc(entries, (size_t)cap * sizeof(*entries));
    }
    entries[count++] = (entry_t){strdup(key), strdup(value)};
}

static void drop(const char *key)
{
    int out = 0;
    for (int i = 0; i < count; i++) {
        if (!strcmp(entries[i].key, key)) {
            free(entries[i].key);
            free(entries[i].value);
        } else {
            entries[out++] = entries[i];
        }
    }
    count = out;
}

static bool given(const char *key, const char *const *cli, int ncli)
{
    for (int i = 0; i < ncli; i++)
        if (!strcmp(cli[i], key))
            return true;
    return false;
}

static bool parse_bool(const char *key, const char *v, bool *out)
{
    if (!strcmp(v, "on") || !strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "1"))
        *out = true;
    else if (!strcmp(v, "off") || !strcmp(v, "no") || !strcmp(v, "false") || !strcmp(v, "0"))
        *out = false;
    else {
        pcsim_log("%s: %s should be on or off, not '%s'", path, key, v);
        return false;
    }
    return true;
}

static void parse_int(const char *key, const char *v, int lo, int hi, int *out)
{
    char *end;
    long n = strtol(v, &end, 10);
    if (*v && !*end && n >= lo && n <= hi)
        *out = (int)n;
    else
        pcsim_log("%s: %s should be a number from %d to %d, not '%s'", path, key, lo, hi, v);
}

// pcsim_opts gets copies, as the entries change while the window runs. They are kept here for as
// long as pcsim runs, since the window points pcsim_opts at its own strings once it starts.
static char **copies;
static int ncopies;

static const char *copy(const char *v)
{
    copies = realloc(copies, (size_t)(ncopies + 1) * sizeof(*copies));
    return copies[ncopies++] = strdup(v);
}

// A path left empty, or "default" where the option has one, means the default.
static const char *path_value(const char *v)
{
    return *v && strcmp(v, "default") != 0 ? copy(v) : NULL;
}

static void apply(const char *key, const char *v)
{
    pcsim_options_t *o = &pcsim_opts;
    if (!strcmp(key, "board"))
        o->default_board = path_value(v);
    else if (!strcmp(key, "rom"))
        o->rom_path = path_value(v);
    else if (!strcmp(key, "flash"))
        o->flash_path = path_value(v);
    else if (!strcmp(key, "fast"))
        parse_bool(key, v, &o->fast);
    else if (!strcmp(key, "unthrottled"))
        parse_bool(key, v, &o->unthrottled);
    else if (!strcmp(key, "trace"))
        parse_bool(key, v, &o->trace);
    else if (!strcmp(key, "console"))
        parse_bool(key, v, &o->console);
    else if (!strcmp(key, "no-wifi"))
        parse_bool(key, v, &o->no_wifi);
    else if (!strcmp(key, "wifi-ap") && o->wifi_ap_count < 16)
        o->wifi_aps[o->wifi_ap_count++] = copy(v);
    else if (!strcmp(key, "forward") && o->forward_count < 16)
        o->forwards[o->forward_count++] = copy(v);
    else if (!strcmp(key, "battery"))
        parse_int(key, v, 0, 100, &o->battery);
    else if (!strcmp(key, "charging"))
        parse_bool(key, v, &o->charging);
    else if (!strcmp(key, "volume"))
        parse_int(key, v, 0, 100, &o->volume);
    else if (!strcmp(key, "mute"))
        parse_bool(key, v, &o->mute);
    else if (!strcmp(key, "scale"))
        parse_int(key, v, 1, 8, &o->scale);
    else if (!strcmp(key, "screenshot-dir") && *v)
        o->screenshot_dir = copy(v);
}

void pcsim_settings_load(const char *const *cli, int ncli)
{
    pcsim_data_path(path, sizeof(path), "settings.ini", false);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    pcsim_log("settings: %s", path);
    char line[2048];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        n++;
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';')
            continue;
        char *eq = strchr(s, '=');
        if (!eq) {
            pcsim_log("%s:%d: expected KEY = VALUE", path, n);
            continue;
        }
        *eq = 0;
        char *key = trim(s), *value = trim(eq + 1);
        if (!is_known(key))
            pcsim_log("%s:%d: unknown setting '%s', kept as it is", path, n, key);
        add(key, value);
    }
    fclose(f);
    // An option on the command line replaces the file's, a list as a whole; the card is left to
    // the caller, which also has --no-card and the default image to weigh.
    for (int i = 0; i < count; i++)
        if (strcmp(entries[i].key, "card") != 0 && !given(entries[i].key, cli, ncli))
            apply(entries[i].key, entries[i].value);
}

const char *pcsim_settings_get(const char *key)
{
    for (int i = 0; i < count; i++)
        if (!strcmp(entries[i].key, key))
            return entries[i].value;
    return NULL;
}

void pcsim_settings_set(const char *key, const char *value)
{
    drop(key);
    if (value)
        add(key, value);
}

void pcsim_settings_set_list(const char *key, const char *const *values, int n)
{
    drop(key);
    for (int i = 0; i < n; i++)
        add(key, values[i]);
}

static void write_entries(FILE *f, const char *key)
{
    for (int i = 0; i < count; i++)
        if (key ? !strcmp(entries[i].key, key) : !is_known(entries[i].key))
            fprintf(f, "%s = %s\n", entries[i].key, entries[i].value);
}

bool pcsim_settings_save(void)
{
    pcsim_data_path(path, sizeof(path), "settings.ini", true);
    char tmp[sizeof(path) + 8];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        pcsim_log("cannot write %s: %s", tmp, strerror(errno));
        return false;
    }
    fputs("# pcsim's window settings, saved as they change there. Each is the long form of a\n"
          "# command-line option, and that option given on the command line wins for the run.\n"
          "# Headless runs ignore this file. pcsim rewrites it, so comments added here are lost.\n",
          f);
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        write_entries(f, known[i]);
    write_entries(f, NULL);
    bool ok = fclose(f) == 0;
    // Written whole and then moved over the old one, so a crash mid-write cannot leave half a file.
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
#else
    ok = ok && rename(tmp, path) == 0;
#endif
    if (!ok) {
        pcsim_log("cannot write %s", path);
        remove(tmp);
    }
    return ok;
}
