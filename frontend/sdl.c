#include "pcsim/devices.h"
#include "pcsim/frontend.h"
#include "pcsim/keys.h"
#include "pcsim/sim.h"

#ifndef PCSIM_HAVE_SDL

bool pcsim_frontend_sdl_available(void)
{
    return false;
}

int pcsim_frontend_sdl_run(void)
{
    return pcsim_frontend_headless_run();
}

#else

#include <string.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

bool pcsim_frontend_sdl_available(void)
{
    return true;
}

typedef struct {
    SDL_Keycode sym;
    uint8_t key;
    bool synth_shift;
} held_t;

static held_t held[32];
static int nheld;

// Host keys the PicoCalc has no key for are sent as the Shift chords its keyboard uses for them.
static bool map_key(SDL_Keycode sym, uint8_t *key, bool *shift)
{
    *shift = false;
    if (sym >= 32 && sym < 127 && sym != SDLK_DELETE) {
        *key = (uint8_t)sym;
        return true;
    }
    switch (sym) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER: *key = PC_KEY_ENTER; return true;
    case SDLK_BACKSPACE: *key = PC_KEY_BACKSPACE; return true;
    case SDLK_TAB: *key = PC_KEY_TAB; return true;
    case SDLK_ESCAPE: *key = PC_KEY_ESC; return true;
    case SDLK_DELETE: *key = PC_KEY_DEL; return true;
    case SDLK_UP: *key = PC_KEY_UP; return true;
    case SDLK_DOWN: *key = PC_KEY_DOWN; return true;
    case SDLK_LEFT: *key = PC_KEY_LEFT; return true;
    case SDLK_RIGHT: *key = PC_KEY_RIGHT; return true;
    case SDLK_F1: *key = PC_KEY_F1; return true;
    case SDLK_F2: *key = PC_KEY_F2; return true;
    case SDLK_F3: *key = PC_KEY_F3; return true;
    case SDLK_F4: *key = PC_KEY_F4; return true;
    case SDLK_F5: *key = PC_KEY_F5; return true;
    case SDLK_CAPSLOCK: *key = PC_KEY_CAPS_LOCK; return true;
    case SDLK_LSHIFT: *key = PC_KEY_LSHIFT; return true;
    case SDLK_RSHIFT: *key = PC_KEY_RSHIFT; return true;
    case SDLK_LCTRL:
    case SDLK_RCTRL: *key = PC_KEY_CTRL; return true;
    case SDLK_LALT:
    case SDLK_RALT: *key = PC_KEY_ALT; return true;
    case SDLK_F6: *key = PC_KEY_F1; *shift = true; return true;
    case SDLK_F7: *key = PC_KEY_F2; *shift = true; return true;
    case SDLK_F8: *key = PC_KEY_F3; *shift = true; return true;
    case SDLK_F9: *key = PC_KEY_F4; *shift = true; return true;
    case SDLK_F10: *key = PC_KEY_F5; *shift = true; return true;
    case SDLK_PAGEUP: *key = PC_KEY_UP; *shift = true; return true;
    case SDLK_PAGEDOWN: *key = PC_KEY_DOWN; *shift = true; return true;
    case SDLK_HOME: *key = PC_KEY_TAB; *shift = true; return true;
    case SDLK_END: *key = PC_KEY_DEL; *shift = true; return true;
    case SDLK_INSERT: *key = PC_KEY_ENTER; *shift = true; return true;
    case SDLK_PAUSE: *key = PC_KEY_ESC; *shift = true; return true;
    default: return false;
    }
}

static void key_down(SDL_Keycode sym)
{
    uint8_t key;
    bool shift;
    if (!map_key(sym, &key, &shift) || nheld == (int)(sizeof(held) / sizeof(held[0])))
        return;
    bool synth = shift && !southbridge_shift_held(pcsim_picocalc.sb);
    if (synth)
        pcsim_keys_press(PC_KEY_LSHIFT);
    pcsim_keys_press(key);
    held[nheld++] = (held_t){sym, key, synth};
}

static void key_up(SDL_Keycode sym)
{
    for (int i = 0; i < nheld; i++) {
        if (held[i].sym != sym)
            continue;
        pcsim_keys_release(held[i].key);
        if (held[i].synth_shift)
            pcsim_keys_release(PC_KEY_LSHIFT);
        held[i] = held[--nheld];
        return;
    }
}

static void release_all(void)
{
    while (nheld)
        key_up(held[nheld - 1].sym);
}

int pcsim_frontend_sdl_run(void)
{
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        pcsim_log("SDL could not start (%s); running headless", SDL_GetError());
        return pcsim_frontend_headless_run();
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    int scale = pcsim_opts.scale;
    SDL_Window *window = SDL_CreateWindow(pcsim_window_title(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          PCSIM_LCD_WIDTH * scale, PCSIM_LCD_HEIGHT * scale,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        pcsim_log("SDL window failed (%s); running headless", SDL_GetError());
        SDL_Quit();
        return pcsim_frontend_headless_run();
    }
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    SDL_RenderSetLogicalSize(renderer, PCSIM_LCD_WIDTH, PCSIM_LCD_HEIGHT);
    SDL_RenderSetIntegerScale(renderer, SDL_TRUE);
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                             PCSIM_LCD_WIDTH, PCSIM_LCD_HEIGHT);
    static uint32_t pixels[PCSIM_LCD_WIDTH * PCSIM_LCD_HEIGHT];

    uint64_t start = pcsim_mono_ns();
    uint32_t last_lcd = ~0u, last_sb = ~0u;
    char last_title[256] = "";
    int code = 0;
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_KEYDOWN:
                if (e.key.repeat)
                    break;
                if (e.key.keysym.sym == SDLK_F11)
                    pcsim_screenshot_auto();
                else if (e.key.keysym.sym == SDLK_F12) {
                    // The BOOTSEL button on the Pico, held while the reset button is pressed.
                    pcsim_bootsel_button((e.key.keysym.mod & KMOD_SHIFT) != 0);
                    pcsim_reboot("reset button");
                }
#ifdef __APPLE__
                else if (e.key.keysym.sym == SDLK_CAPSLOCK) {
                    pcsim_keys_press(PC_KEY_CAPS_LOCK);
                    pcsim_keys_release(PC_KEY_CAPS_LOCK);
                }
#endif
                else
                    key_down(e.key.keysym.sym);
                break;
            case SDL_KEYUP:
#ifdef __APPLE__
                // macOS reports Caps Lock as down when it latches and up when it unlatches, so
                // each report is one press of the PicoCalc's key.
                if (e.key.keysym.sym == SDLK_CAPSLOCK) {
                    pcsim_keys_press(PC_KEY_CAPS_LOCK);
                    pcsim_keys_release(PC_KEY_CAPS_LOCK);
                    break;
                }
#endif
                key_up(e.key.keysym.sym);
                break;
            case SDL_DROPFILE: {
                char err[256];
                if (pcsim_flash_uf2(e.drop.file, err, sizeof(err)))
                    pcsim_log("flashing %s", e.drop.file);
                else
                    pcsim_log("%s", err);
                SDL_free(e.drop.file);
                break;
            }
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                    release_all();
                break;
            default:
                break;
            }
        }

        southbridge_tick(pcsim_picocalc.sb);
        if (pcsim_quit_requested(&code))
            break;
        if (pcsim_opts.timeout_s > 0 && (pcsim_mono_ns() - start) / 1e9 >= pcsim_opts.timeout_s)
            break;

        uint32_t lcd_gen = st7365p_generation(pcsim_picocalc.lcd);
        uint32_t sb_gen = southbridge_generation(pcsim_picocalc.sb);
        if (lcd_gen != last_lcd || sb_gen != last_sb) {
            last_lcd = lcd_gen;
            last_sb = sb_gen;
            st7365p_render(pcsim_picocalc.lcd, pixels, southbridge_lcd_backlight(pcsim_picocalc.sb));
            SDL_UpdateTexture(texture, NULL, pixels, PCSIM_LCD_WIDTH * 4);
        }
        const char *title = pcsim_window_title();
        if (strcmp(title, last_title) != 0) {
            snprintf(last_title, sizeof(last_title), "%s", title);
            SDL_SetWindowTitle(window, title);
        }
        SDL_SetRenderDrawColor(renderer, 24, 24, 24, 255);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
        SDL_Delay(8);
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return code;
}

#endif
