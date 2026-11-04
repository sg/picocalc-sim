#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int pcsim_frontend_sdl_run(void);
int pcsim_frontend_headless_run(void);
bool pcsim_frontend_sdl_available(void);

void pcsim_script_start(const char *path);

bool pcsim_png_write(const char *path, const uint32_t *argb, int width, int height);
bool pcsim_screenshot(const char *path);
// A timestamped PNG in the screenshot directory; returns the path written, or NULL.
const char *pcsim_screenshot_auto(void);

// Physical key presses on the PicoCalc keyboard, as a person would make them.
void pcsim_keys_press(uint8_t key);
void pcsim_keys_release(uint8_t key);
// Types one character, pressing Shift around it when the PicoCalc needs it; false if it has no key.
bool pcsim_keys_type_char(char c, unsigned hold_ms, unsigned gap_ms);
// Presses a chord such as "ctrl+alt+del", "shift+up", "brk" or "enter".
bool pcsim_keys_chord(const char *spec, unsigned hold_ms);

const char *pcsim_window_title(void);

// The window's settings file in the data directory. Loading sets pcsim_opts from it, except for
// the long options named in cli, which the command line gave.
void pcsim_settings_load(const char *const *cli, int ncli);
const char *pcsim_settings_get(const char *key);
// A NULL value removes the setting.
void pcsim_settings_set(const char *key, const char *value);
void pcsim_settings_set_list(const char *key, const char *const *values, int n);
bool pcsim_settings_save(void);

#ifdef __cplusplus
}
#endif
