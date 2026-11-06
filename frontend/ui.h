#pragma once

#include <stdbool.h>

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_init(SDL_Window *window, SDL_Renderer *renderer);
void ui_shutdown(void);
// Sizes the window for the LCD at a whole-number scale plus the menu bar, and the terminal beside
// it when that is shown.
void ui_fit_window(int scale);
// The width the terminal takes at the window's right, in render pixels.
float ui_side_width(void);
// True when the event belongs to the UI, such as a key typed into one of its text fields.
bool ui_event(const SDL_Event *e);
// True while a text field or the terminal has the keyboard rather than the PicoCalc.
bool ui_wants_keyboard(void);
void ui_drop_file(const char *path);
// Saves a screenshot and shows over the LCD where it went, or why it failed.
void ui_screenshot(void);
// Must come before every restart, so that a failed one can show what went wrong.
void ui_restarting(void);
// Lays out this frame's UI and returns the height of the menu bar in render pixels.
float ui_frame(void);
void ui_render(void);

#ifdef __cplusplus
}
#endif
