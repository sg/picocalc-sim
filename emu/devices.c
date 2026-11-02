#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "chip.h"
#include "pcsim/bus.h"
#include "pcsim/sim.h"

// Connects device models to the chip's pins. An SPI device takes whole frames from whichever
// PL022 drives its clock pin, and decodes bits itself when PIO or software toggles the pins.

static pcsim_spi_device_t *spi_devs;
static pcsim_i2c_device_t *i2c_devs;
static gpio_watch_t *watches[NUM_GPIO_MAX];

void pcsim_spi_attach(pcsim_spi_device_t *dev)
{
    dev->next = spi_devs;
    spi_devs = dev;
}

void pcsim_i2c_attach(pcsim_i2c_device_t *dev)
{
    dev->next = i2c_devs;
    i2c_devs = dev;
}

void pcsim_gpio_watch(unsigned pin, pcsim_gpio_watch_fn fn, void *ctx)
{
    if (pin >= NUM_GPIO_MAX)
        return;
    gpio_watch_t *w = calloc(1, sizeof(*w));
    w->fn = fn;
    w->ctx = ctx;
    w->next = watches[pin];
    watches[pin] = w;
}

void pcsim_gpio_drive_input(unsigned pin, int level)
{
    chip_t *m = g_chip;
    if (!m || pin >= NUM_GPIO_MAX)
        return;
    pcsim_machine_lock();
    m->gpio[pin].ext_driven = level >= 0;
    m->gpio[pin].ext_level = level > 0;
    gpio_refresh(m);
    pcsim_machine_unlock();
}

bool pcsim_gpio_level(unsigned pin)
{
    chip_t *m = g_chip;
    if (!m || pin >= NUM_GPIO_MAX)
        return false;
    return m->gpio[pin].level;
}

static bool spi_pins_on_controller(chip_t *m, pcsim_spi_device_t *d, int bus)
{
    return gpio_func(m, d->sck_pin) == FUNC_SPI && (int)((d->sck_pin >> 3) & 1) == bus;
}

void devices_spi_frame(chip_t *m, int bus, uint16_t mosi, int bits, uint16_t *miso)
{
    uint16_t in = 0xffff;
    for (pcsim_spi_device_t *d = spi_devs; d; d = d->next) {
        if (!spi_pins_on_controller(m, d, bus) || m->gpio[d->cs_pin].level)
            continue;
        if (bits > 8) {
            uint8_t hi = d->xfer(d->ctx, (uint8_t)(mosi >> (bits - 8)));
            uint8_t lo = d->xfer(d->ctx, (uint8_t)(mosi << (16 - bits) >> 8));
            in &= (uint16_t)(((hi << 8) | lo) >> (16 - bits));
        } else {
            in &= d->xfer(d->ctx, (uint8_t)(mosi << (8 - bits)));
        }
    }
    *miso = in;
}

void devices_gpio_changed(chip_t *m, unsigned pin, bool level)
{
    for (pcsim_spi_device_t *d = spi_devs; d; d = d->next) {
        if (pin == d->cs_pin) {
            bool sel = !level;
            if (sel != d->selected) {
                d->selected = sel;
                d->nbits = 0;
                if (d->select)
                    d->select(d->ctx, sel);
            }
        } else if (pin == d->sck_pin && level && d->selected && gpio_func(m, d->sck_pin) != FUNC_SPI) {
            d->shift = (uint8_t)((d->shift << 1) | (m->gpio[d->mosi_pin].level ? 1 : 0));
            if (++d->nbits == 8) {
                d->nbits = 0;
                d->xfer(d->ctx, d->shift);
            }
        }
    }
    for (gpio_watch_t *w = watches[pin]; w; w = w->next)
        w->fn(w->ctx, pin, level);
}

void devices_spi_bit(chip_t *m, unsigned sck_pin, unsigned mosi_pin, int bit)
{
    (void)m;
    for (pcsim_spi_device_t *d = spi_devs; d; d = d->next) {
        if (d->sck_pin != sck_pin || d->mosi_pin != mosi_pin || !d->selected)
            continue;
        d->shift = (uint8_t)((d->shift << 1) | (bit & 1));
        if (++d->nbits == 8) {
            d->nbits = 0;
            d->xfer(d->ctx, d->shift);
        }
    }
}

int devices_i2c_start(chip_t *m, int bus, uint8_t addr, void **handle)
{
    for (pcsim_i2c_device_t *d = i2c_devs; d; d = d->next) {
        if (d->addr != addr)
            continue;
        if (gpio_func(m, d->scl_pin) != FUNC_I2C || (int)((d->scl_pin >> 1) & 1) != bus)
            continue;
        if (gpio_func(m, d->sda_pin) != FUNC_I2C)
            continue;
        *handle = d;
        return 1;
    }
    return 0;
}

void devices_i2c_write(void *handle, const uint8_t *buf, int len, bool nostop)
{
    pcsim_i2c_device_t *d = handle;
    d->write(d->ctx, buf, (size_t)len, nostop);
}

int devices_i2c_read(void *handle, uint8_t *buf, int len, bool nostop)
{
    pcsim_i2c_device_t *d = handle;
    return d->read(d->ctx, buf, (size_t)len, nostop);
}

void devices_uart_tx(chip_t *m, int idx, uint8_t byte)
{
    (void)m;
    pcsim_console_out(idx, byte);
}
