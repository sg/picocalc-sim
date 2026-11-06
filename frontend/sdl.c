#include "picocalc-sim/devices.h"
#include "picocalc-sim/frontend.h"
#include "picocalc-sim/keys.h"
#include "picocalc-sim/sim.h"

#ifndef PICOCALC_SIM_HAVE_SDL

bool picocalc_sim_frontend_sdl_available(void)
{
    return false;
}

int picocalc_sim_frontend_sdl_run(void)
{
    return picocalc_sim_frontend_headless_run();
}

#else

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "ui.h"

bool picocalc_sim_frontend_sdl_available(void)
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
    bool synth = shift && !southbridge_shift_held(picocalc_sim_picocalc.sb);
    if (synth)
        picocalc_sim_keys_press(PC_KEY_LSHIFT);
    picocalc_sim_keys_press(key);
    held[nheld++] = (held_t){sym, key, synth};
}

static void key_up(SDL_Keycode sym)
{
    for (int i = 0; i < nheld; i++) {
        if (held[i].sym != sym)
            continue;
        picocalc_sim_keys_release(held[i].key);
        if (held[i].synth_shift)
            picocalc_sim_keys_release(PC_KEY_LSHIFT);
        held[i] = held[--nheld];
        return;
    }
}

static void release_all(void)
{
    while (nheld)
        key_up(held[nheld - 1].sym);
}

// A key held through the power switch is down as the PicoCalc starts, which is how uf2loader is
// asked for its menu, so keys still held on the host are pressed again on the new machine.
static void press_held(const SDL_Keycode *syms, int n)
{
    const bool *down = SDL_GetKeyboardState(NULL);
    for (int i = 0; i < n; i++)
        if (down[SDL_GetScancodeFromKey(syms[i], NULL)])
            key_down(syms[i]);
}

static void SDLCALL audio_cb(void *ctx, SDL_AudioStream *stream, int additional, int total)
{
    static int16_t buf[2 * 1024];
    int frames = (additional + 3) / 4;
    int gain = picocalc_sim_opts.volume * 256 / 100;
    while (frames > 0) {
        int n = frames < 1024 ? frames : 1024;
        picocalc_audio_pull(picocalc_sim_picocalc.audio, buf, (size_t)n);
        for (int i = 0; i < 2 * n; i++)
            buf[i] = (int16_t)(buf[i] * gain / 256);
        SDL_PutAudioStreamData(stream, buf, n * 4);
        frames -= n;
    }
}

static SDL_AudioStream *open_audio(void)
{
    if (picocalc_sim_opts.mute || !picocalc_sim_machine_running())
        return NULL;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        picocalc_sim_log("no sound: %s", SDL_GetError());
        return NULL;
    }
    // SDL3 would pick 1024 frames at 48 kHz; 512 keeps the sound closer behind the picture.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
    SDL_AudioSpec spec = {.format = SDL_AUDIO_S16, .channels = 2, .freq = 48000};
    SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, NULL);
    if (!stream) {
        picocalc_sim_log("no sound: %s", SDL_GetError());
        return NULL;
    }
    if (!picocalc_audio_play(picocalc_sim_picocalc.audio)) {
        SDL_DestroyAudioStream(stream);
        return NULL;
    }
    SDL_ResumeAudioStreamDevice(stream);
    return stream;
}

static SDL_Window *create_window(void)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, picocalc_sim_window_title());
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, PICOCALC_SIM_LCD_WIDTH);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, PICOCALC_SIM_LCD_HEIGHT);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    // shown once the UI has worked out its size
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    SDL_Window *window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    return window;
}

static SDL_FRect lcd_rect(SDL_Renderer *renderer, float top)
{
    int w, h;
    SDL_GetCurrentRenderOutputSize(renderer, &w, &h);
    float aw = fmaxf((float)w - ui_side_width(), 0.0f), ah = (float)h - top;
    float fit = fminf(aw / PICOCALC_SIM_LCD_WIDTH, ah / PICOCALC_SIM_LCD_HEIGHT);
    float scale = fit >= 1.0f ? floorf(fit) : fmaxf(fit, 0.0f);
    float lw = PICOCALC_SIM_LCD_WIDTH * scale, lh = PICOCALC_SIM_LCD_HEIGHT * scale;
    return (SDL_FRect){floorf((aw - lw) / 2), top + floorf((ah - lh) / 2), lw, lh};
}

int picocalc_sim_frontend_sdl_run(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        picocalc_sim_log("SDL could not start (%s); running headless", SDL_GetError());
        return picocalc_sim_frontend_headless_run();
    }
    SDL_Window *window = create_window();
    if (!window) {
        picocalc_sim_log("SDL window failed (%s); running headless", SDL_GetError());
        SDL_Quit();
        return picocalc_sim_frontend_headless_run();
    }
    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
    SDL_SetRenderVSync(renderer, 1);
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                             PICOCALC_SIM_LCD_WIDTH, PICOCALC_SIM_LCD_HEIGHT);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    static uint32_t pixels[PICOCALC_SIM_LCD_WIDTH * PICOCALC_SIM_LCD_HEIGHT];
    ui_init(window, renderer);
    ui_fit_window(picocalc_sim_opts.scale);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);
    SDL_AudioStream *audio = open_audio();
    bool muted = picocalc_sim_opts.mute;

    uint64_t start = picocalc_sim_mono_ns();
    uint32_t last_lcd = ~0u, last_sb = ~0u;
    char last_title[256] = "";
    int code = 0;
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            bool ui_took = ui_event(&e);
            switch (e.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.repeat || ui_took || !picocalc_sim_machine_running())
                    break;
                if (e.key.key == SDLK_F11)
                    ui_screenshot();
                else if (e.key.key == SDLK_F12) {
                    // The BOOTSEL button on the Pico, held while the reset button is pressed.
                    picocalc_sim_bootsel_button((e.key.mod & SDL_KMOD_SHIFT) != 0);
                    picocalc_sim_reboot("reset button");
                } else
                    key_down(e.key.key);
                break;
            case SDL_EVENT_KEY_UP:
                // even when the UI has the keyboard, a key the PicoCalc saw go down must come up
                key_up(e.key.key);
                break;
            case SDL_EVENT_DROP_FILE:
                ui_drop_file(e.drop.data);
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                release_all();
                break;
            default:
                break;
            }
        }

        if (picocalc_sim_restart_requested()) {
            SDL_Keycode carried[sizeof(held) / sizeof(held[0])];
            int ncarried = nheld;
            for (int i = 0; i < nheld; i++)
                carried[i] = held[i].sym;
            // The devices are about to be freed, so nothing may hold a key or pull sound from them.
            release_all();
            if (audio)
                SDL_DestroyAudioStream(audio);
            audio = NULL;
            ui_restarting();
            // A machine that fails to start stays stopped, and the UI says why.
            if (picocalc_sim_machine_restart()) {
                press_held(carried, ncarried);
                audio = open_audio();
            }
            last_lcd = last_sb = ~0u;
        }
        if (picocalc_sim_opts.mute != muted) {
            muted = picocalc_sim_opts.mute;
            if (audio)
                SDL_DestroyAudioStream(audio);
            audio = open_audio();
        }
        if (picocalc_sim_machine_running())
            southbridge_tick(picocalc_sim_picocalc.sb);
        if (picocalc_sim_quit_requested(&code))
            break;
        if (picocalc_sim_opts.timeout_s > 0 && (picocalc_sim_mono_ns() - start) / 1e9 >= picocalc_sim_opts.timeout_s)
            break;

        bool on = picocalc_sim_machine_running();
        uint32_t lcd_gen = on ? st7365p_generation(picocalc_sim_picocalc.lcd) : 0;
        uint32_t sb_gen = on ? southbridge_generation(picocalc_sim_picocalc.sb) : 0;
        if (on && (lcd_gen != last_lcd || sb_gen != last_sb)) {
            last_lcd = lcd_gen;
            last_sb = sb_gen;
            st7365p_render(picocalc_sim_picocalc.lcd, pixels, southbridge_lcd_backlight(picocalc_sim_picocalc.sb));
            SDL_UpdateTexture(texture, NULL, pixels, PICOCALC_SIM_LCD_WIDTH * 4);
        }
        const char *title = picocalc_sim_window_title();
        if (strcmp(title, last_title) != 0) {
            snprintf(last_title, sizeof(last_title), "%s", title);
            SDL_SetWindowTitle(window, title);
        }
        float top = ui_frame();
        SDL_SetRenderDrawColor(renderer, 24, 24, 24, 255);
        SDL_RenderClear(renderer);
        if (on) {
            SDL_FRect dst = lcd_rect(renderer, top);
            SDL_RenderTexture(renderer, texture, NULL, &dst);
        }
        ui_render();
        SDL_RenderPresent(renderer);
        SDL_Delay(8);
    }

    if (audio)
        SDL_DestroyAudioStream(audio);
    ui_shutdown();
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return code;
}

#endif
