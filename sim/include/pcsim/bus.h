#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What device models see of the chip: pins, and the SPI and I2C buses those pins carry.

typedef void (*pcsim_gpio_watch_fn)(void *ctx, unsigned pin, bool level);

void pcsim_gpio_watch(unsigned pin, pcsim_gpio_watch_fn fn, void *ctx);

// Gives one device that sees pins through an analog filter (PWM audio) the fraction of each
// 1/rate_hz of emulated time each pin spent high, n windows of npins values per call, on the
// emulation thread a little behind the emulated present.
typedef void (*pcsim_pin_wave_fn)(void *ctx, const float *frac, size_t n);
bool pcsim_pin_wave(const unsigned *pins, int npins, unsigned rate_hz, pcsim_pin_wave_fn fn, void *ctx);

// Drives an input pin from outside the chip: 0 or 1, or -1 to let it float to its pull.
void pcsim_gpio_drive_input(unsigned pin, int level);
bool pcsim_gpio_level(unsigned pin);
// The same from a device's own watch callback, which runs with the machine already locked.
void pcsim_gpio_drive_input_locked(unsigned pin, int level);
// In a watch callback: when the change being reported reached the pin, in picoseconds of emulated
// time, so a device can answer it as late as the real part would.
uint64_t pcsim_gpio_edge_time(void);
// From a watch callback: drives an input pin whose level changes at the pin at time t.
void pcsim_gpio_drive_input_at(unsigned pin, int level, uint64_t t);
// How sharp the pin's output edges are: 0 for the default 4 mA slow slew pad to 4 for 12 mA fast.
int pcsim_gpio_pad_speed(unsigned pin);

typedef struct pcsim_spi_device {
    const char *name;
    unsigned sck_pin, mosi_pin, miso_pin, cs_pin;
    void (*select)(void *ctx, bool selected);
    uint8_t (*xfer)(void *ctx, uint8_t mosi);
    void *ctx;
    struct pcsim_spi_device *next;
    bool selected;
    uint8_t shift;
    int nbits;
} pcsim_spi_device_t;

// The device answers whichever SPI controller, PIO program or bit-banging drives its pins. The
// machine frees dev, which must come from malloc, when it stops.
void pcsim_spi_attach(pcsim_spi_device_t *dev);

typedef struct pcsim_i2c_device {
    const char *name;
    unsigned sda_pin, scl_pin;
    uint8_t addr;
    // Return the number of bytes acknowledged, or a negative value for a NAK on the address.
    int (*write)(void *ctx, const uint8_t *src, size_t len, bool nostop);
    int (*read)(void *ctx, uint8_t *dst, size_t len, bool nostop);
    void *ctx;
    struct pcsim_i2c_device *next;
} pcsim_i2c_device_t;

// As for SPI, the machine frees dev when it stops.
void pcsim_i2c_attach(pcsim_i2c_device_t *dev);

#ifdef __cplusplus
}
#endif
