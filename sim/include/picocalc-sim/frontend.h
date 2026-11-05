#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int picocalc_sim_frontend_sdl_run(void);
int picocalc_sim_frontend_headless_run(void);
bool picocalc_sim_frontend_sdl_available(void);

void picocalc_sim_script_start(const char *path);

bool picocalc_sim_png_write(const char *path, const uint32_t *argb, int width, int height);
bool picocalc_sim_screenshot(const char *path);
// A timestamped PNG in the screenshot directory; returns the path written, or NULL.
const char *picocalc_sim_screenshot_auto(void);

// Physical key presses on the PicoCalc keyboard, as a person would make them.
void picocalc_sim_keys_press(uint8_t key);
void picocalc_sim_keys_release(uint8_t key);
// Types one character, pressing Shift around it when the PicoCalc needs it; false if it has no key.
bool picocalc_sim_keys_type_char(char c, unsigned hold_ms, unsigned gap_ms);
// Presses a chord such as "ctrl+alt+del", "shift+up", "brk" or "enter".
bool picocalc_sim_keys_chord(const char *spec, unsigned hold_ms);

const char *picocalc_sim_window_title(void);

// The window's settings file in the data directory. Loading sets picocalc_sim_opts from it, except
// for the long options named in cli, which the command line gave.
void picocalc_sim_settings_load(const char *const *cli, int ncli);
const char *picocalc_sim_settings_get(const char *key);
// A NULL value removes the setting.
void picocalc_sim_settings_set(const char *key, const char *value);
void picocalc_sim_settings_set_list(const char *key, const char *const *values, int n);
bool picocalc_sim_settings_save(void);

#ifdef __cplusplus
}
#endif
