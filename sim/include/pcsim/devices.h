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

typedef struct sdcard sdcard_t;
sdcard_t *sdcard_create(pcsim_spi_pins_t pins, int detect_pin);
bool sdcard_insert(sdcard_t *sd, const char *image_path);
void sdcard_eject(sdcard_t *sd);
void sdcard_flush(sdcard_t *sd);
bool sdcard_present(sdcard_t *sd);

typedef struct picocalc {
    st7365p_t *lcd;
    southbridge_t *sb;
    sdcard_t *sd;
} picocalc_t;

extern picocalc_t pcsim_picocalc;
void picocalc_board_init(void);

#ifdef __cplusplus
}
#endif
