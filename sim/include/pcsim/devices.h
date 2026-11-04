#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCSIM_LCD_WIDTH 320
#define PCSIM_LCD_HEIGHT 320

typedef struct {
    unsigned sck, mosi, miso, cs;
} pcsim_spi_pins_t;

typedef struct st7365p st7365p_t;
st7365p_t *st7365p_create(pcsim_spi_pins_t pins, unsigned dc_pin, unsigned rst_pin);
// Composes what the panel shows (scroll, inversion, sleep, backlight) into 320x320 ARGB8888.
void st7365p_render(st7365p_t *lcd, uint32_t *argb, uint8_t backlight);
uint32_t st7365p_generation(st7365p_t *lcd);
void st7365p_destroy(st7365p_t *lcd);

typedef struct southbridge southbridge_t;
southbridge_t *southbridge_create(unsigned sda_pin, unsigned scl_pin, uint8_t addr);
// key is the unshifted code of a physical key; the model applies Shift and Caps Lock itself.
void southbridge_key(southbridge_t *sb, uint8_t key, bool pressed);
void southbridge_tick(southbridge_t *sb);
uint8_t southbridge_lcd_backlight(southbridge_t *sb);
uint8_t southbridge_kbd_backlight(southbridge_t *sb);
bool southbridge_caps_lock(southbridge_t *sb);
bool southbridge_shift_held(southbridge_t *sb);
void southbridge_set_battery(southbridge_t *sb, uint8_t percent, bool charging);
uint32_t southbridge_generation(southbridge_t *sb);
void southbridge_destroy(southbridge_t *sb);

typedef struct sdcard sdcard_t;
sdcard_t *sdcard_create(pcsim_spi_pins_t pins, int detect_pin);
bool sdcard_insert(sdcard_t *sd, const char *image_path);
void sdcard_eject(sdcard_t *sd);
void sdcard_flush(sdcard_t *sd);
bool sdcard_present(sdcard_t *sd);
// Closes the image without the card-detect pin seeing it leave.
void sdcard_destroy(sdcard_t *sd);

// The mainboard's filters on the two audio pins, as 48 kHz stereo; it costs nothing until it is
// recorded or played.
typedef struct picocalc_audio picocalc_audio_t;
picocalc_audio_t *picocalc_audio_create(unsigned left_pin, unsigned right_pin);
// Writes everything from now on to a WAV file, in emulated time, ending any earlier recording.
bool picocalc_audio_record(picocalc_audio_t *a, const char *wav_path);
bool picocalc_audio_recording(picocalc_audio_t *a);
void picocalc_audio_close(picocalc_audio_t *a);
// Starts filling the playback buffer that picocalc_audio_pull empties, from the host's audio thread.
bool picocalc_audio_play(picocalc_audio_t *a);
// Fills n stereo frames, padding with silence while the buffer builds up; returns frames that
// came from the device.
size_t picocalc_audio_pull(picocalc_audio_t *a, int16_t *frames, size_t n);
// Finishes any recording; nothing may be pulling.
void picocalc_audio_destroy(picocalc_audio_t *a);

// The mainboard's PSRAM, an ESP-PSRAM64H on GPIOs; sio_pins are SIO0 (SI) to SIO3.
typedef struct esp_psram esp_psram_t;
esp_psram_t *esp_psram_create(unsigned cs_pin, unsigned sck_pin, const unsigned sio_pins[4]);
void esp_psram_destroy(esp_psram_t *p);

typedef struct picocalc {
    st7365p_t *lcd;
    southbridge_t *sb;
    sdcard_t *sd;
    picocalc_audio_t *audio;
    esp_psram_t *psram;
} picocalc_t;

extern picocalc_t pcsim_picocalc;
bool picocalc_board_init(void);
// Frees the devices picocalc_board_init made; the machine drops their pin registrations.
void picocalc_board_free(void);

#ifdef __cplusplus
}
#endif
