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
// Drives an input pin from outside the chip: 0 or 1, or -1 to let it float to its pull.
void pcsim_gpio_drive_input(unsigned pin, int level);
bool pcsim_gpio_level(unsigned pin);

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

// The device answers whichever SPI controller, PIO program or bit-banging drives its pins.
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

void pcsim_i2c_attach(pcsim_i2c_device_t *dev);

#ifdef __cplusplus
}
#endif
