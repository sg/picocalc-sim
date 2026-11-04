#include "pcsim/devices.h"
#include "pcsim/sim.h"

// The PicoCalc mainboard's wiring to the Pico socket, common to every firmware for it.

picocalc_t pcsim_picocalc;

bool picocalc_board_init(void)
{
    pcsim_picocalc.lcd = st7365p_create((pcsim_spi_pins_t){.sck = 10, .mosi = 11, .miso = 12, .cs = 13}, 14, 15);
    pcsim_picocalc.sb = southbridge_create(6, 7, 0x1f);
    pcsim_picocalc.sd = sdcard_create((pcsim_spi_pins_t){.sck = 18, .mosi = 19, .miso = 16, .cs = 17}, 22);
    southbridge_set_battery(pcsim_picocalc.sb, (uint8_t)pcsim_opts.battery, pcsim_opts.charging);
    pcsim_picocalc.audio = picocalc_audio_create(26, 27);
    pcsim_picocalc.psram = esp_psram_create(20, 21, (const unsigned[4]){2, 3, 4, 5});
    return !pcsim_opts.wav_path || picocalc_audio_record(pcsim_picocalc.audio, pcsim_opts.wav_path);
}

void picocalc_board_free(void)
{
    picocalc_t *b = &pcsim_picocalc;
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
