#include "pcsim/devices.h"
#include "pcsim/sim.h"

// The PicoCalc mainboard's wiring to the Pico socket, common to every firmware for it.

picocalc_t pcsim_picocalc;

bool picocalc_board_init(void)
{
    if (pcsim_picocalc.lcd)
        return true;
    pcsim_picocalc.lcd = st7365p_create((pcsim_spi_pins_t){.sck = 10, .mosi = 11, .miso = 12, .cs = 13}, 14, 15);
    pcsim_picocalc.sb = southbridge_create(6, 7, 0x1f);
    pcsim_picocalc.sd = sdcard_create((pcsim_spi_pins_t){.sck = 18, .mosi = 19, .miso = 16, .cs = 17}, 22);
    southbridge_set_battery(pcsim_picocalc.sb, (uint8_t)pcsim_opts.battery, pcsim_opts.charging);
    pcsim_picocalc.audio = picocalc_audio_create(26, 27);
    return !pcsim_opts.wav_path || picocalc_audio_record(pcsim_picocalc.audio, pcsim_opts.wav_path);
}
