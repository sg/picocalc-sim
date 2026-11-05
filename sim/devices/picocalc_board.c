#include "picocalc-sim/devices.h"
#include "picocalc-sim/sim.h"

// The PicoCalc mainboard's wiring to the Pico socket, common to every firmware for it.

picocalc_t picocalc_sim_picocalc;

bool picocalc_board_init(void)
{
    picocalc_sim_picocalc.lcd = st7365p_create((picocalc_sim_spi_pins_t){.sck = 10, .mosi = 11, .miso = 12, .cs = 13}, 14, 15);
    picocalc_sim_picocalc.sb = southbridge_create(6, 7, 0x1f);
    picocalc_sim_picocalc.sd = sdcard_create((picocalc_sim_spi_pins_t){.sck = 18, .mosi = 19, .miso = 16, .cs = 17}, 22);
    southbridge_set_battery(picocalc_sim_picocalc.sb, (uint8_t)picocalc_sim_opts.battery, picocalc_sim_opts.charging);
    picocalc_sim_picocalc.audio = picocalc_audio_create(26, 27);
    picocalc_sim_picocalc.psram = esp_psram_create(20, 21, (const unsigned[4]){2, 3, 4, 5});
    return !picocalc_sim_opts.wav_path || picocalc_audio_record(picocalc_sim_picocalc.audio, picocalc_sim_opts.wav_path);
}

void picocalc_board_free(void)
{
    picocalc_t *b = &picocalc_sim_picocalc;
    if (b->lcd)
        st7365p_destroy(b->lcd);
    if (b->sb)
        southbridge_destroy(b->sb);
    if (b->sd)
        sdcard_destroy(b->sd);
    if (b->audio)
        picocalc_audio_destroy(b->audio);
    if (b->psram)
        esp_psram_destroy(b->psram);
    *b = (picocalc_t){0};
}
