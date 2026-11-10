#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "chip.h"
#include "picocalc-sim/bus.h"
#include "picocalc-sim/sim.h"

// Connects device models to the chip's pins. An SPI device takes whole frames from whichever
// PL022 drives its clock pin, and decodes bits itself when PIO or software toggles the pins.

static picocalc_sim_spi_device_t *spi_devs;
static picocalc_sim_i2c_device_t *i2c_devs;
static gpio_watch_t *watches[NUM_GPIO_MAX];

void devices_detach_all(void)
{
    while (spi_devs) {
        picocalc_sim_spi_device_t *d = spi_devs;
        spi_devs = d->next;
        free(d);
    }
    while (i2c_devs) {
        picocalc_sim_i2c_device_t *d = i2c_devs;
        i2c_devs = d->next;
        free(d);
    }
    for (int pin = 0; pin < NUM_GPIO_MAX; pin++)
        while (watches[pin]) {
            gpio_watch_t *w = watches[pin];
            watches[pin] = w->next;
            free(w);
        }
    pinwave_stop();
}

void picocalc_sim_spi_attach(picocalc_sim_spi_device_t *dev)
{
    dev->next = spi_devs;
    spi_devs = dev;
}

void picocalc_sim_i2c_attach(picocalc_sim_i2c_device_t *dev)
{
    dev->next = i2c_devs;
    i2c_devs = dev;
}

void picocalc_sim_gpio_watch(unsigned pin, picocalc_sim_gpio_watch_fn fn, void *ctx)
{
    if (pin >= NUM_GPIO_MAX)
        return;
    gpio_watch_t *w = calloc(1, sizeof(*w));
    w->fn = fn;
    w->ctx = ctx;
    w->next = watches[pin];
    watches[pin] = w;
}

void picocalc_sim_gpio_drive_input(unsigned pin, int level)
{
    chip_t *m = g_chip;
    if (!m || pin >= NUM_GPIO_MAX)
        return;
    picocalc_sim_machine_lock();
    m->gpio[pin].ext_driven = level >= 0;
    m->gpio[pin].ext_level = level > 0;
    gpio_refresh(m);
    picocalc_sim_machine_unlock();
}

void picocalc_sim_gpio_drive_input_locked(unsigned pin, int level)
{
    chip_t *m = g_chip;
    if (!m || pin >= NUM_GPIO_MAX)
        return;
    gpio_pin_t *g = &m->gpio[pin];
    bool drive = level >= 0, lvl = level > 0;
    if (g->ext_driven == drive && g->ext_level == lvl)
        return;
    g->ext_driven = drive;
    g->ext_level = lvl;
    gpio_refresh_mask(m, 1ull << pin);
}

uint64_t picocalc_sim_gpio_edge_time(void)
{
    chip_t *m = g_chip;
    return m->pin_t ? m->pin_t : m->edge_t ? m->edge_t : m->now;
}

void picocalc_sim_gpio_drive_input_at(unsigned pin, int level, uint64_t t)
{
    chip_t *m = g_chip;
    if (!m)
        return;
    uint64_t at = m->ext_at;
    m->ext_at = t;
    picocalc_sim_gpio_drive_input_locked(pin, level);
    m->ext_at = at;
}

int picocalc_sim_gpio_pad_speed(unsigned pin)
{
    chip_t *m = g_chip;
    return m && pin < NUM_GPIO_MAX ? gpio_pad_speed(m, pin) : 0;
}

bool picocalc_sim_gpio_level(unsigned pin)
{
    chip_t *m = g_chip;
    if (!m || pin >= NUM_GPIO_MAX)
        return false;
    return m->gpio[pin].level;
}

static bool spi_pins_on_controller(chip_t *m, picocalc_sim_spi_device_t *d, int bus)
{
    return gpio_func(m, d->sck_pin) == FUNC_SPI && (int)((d->sck_pin >> 3) & 1) == bus;
}

void devices_spi_frame(chip_t *m, int bus, uint16_t mosi, int bits, uint16_t *miso)
{
    uint16_t in = 0xffff;
    for (picocalc_sim_spi_device_t *d = spi_devs; d; d = d->next) {
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

enum { BB_IDLE, BB_ADDR, BB_ADDR_ACK, BB_WRITE, BB_WRITE_ACK, BB_READ, BB_READ_ACK };

static void bb_hold(picocalc_sim_i2c_device_t *d, bool low)
{
    if (d->holding == low)
        return;
    d->holding = low;
    picocalc_sim_gpio_drive_input_locked(d->sda_pin, low ? 0 : -1);
}

static void bb_send_bit(picocalc_sim_i2c_device_t *d)
{
    uint8_t b = d->pos < sizeof(d->buf) ? d->buf[d->pos] : 0xff;
    bb_hold(d, !((b >> (7 - d->nbits)) & 1));
}

// Releasing SDA can come straight back here as a STOP, so the transfer is over before it does.
static void bb_finish(picocalc_sim_i2c_device_t *d, bool nostop)
{
    size_t len = d->state == BB_WRITE || d->state == BB_WRITE_ACK ? d->len : 0;
    d->state = BB_IDLE;
    d->len = 0;
    if (len)
        d->write(d->ctx, d->buf, len, nostop);
    bb_hold(d, false);
}

// Software I2C, such as arduino-pico's probe of an address with an empty write, toggles SDA and SCL
// as plain GPIOs. The device follows the bus at its pins then, and answers by holding SDA low, as
// the real part does.
static void i2c_pin_changed(chip_t *m, picocalc_sim_i2c_device_t *d, unsigned pin, bool level)
{
    if (gpio_func(m, d->scl_pin) == FUNC_I2C || gpio_func(m, d->sda_pin) == FUNC_I2C) {
        bb_hold(d, false);
        d->state = BB_IDLE;
        return;
    }
    bool scl = m->gpio[d->scl_pin].level, sda = m->gpio[d->sda_pin].level;
    if (pin == d->sda_pin) {
        if (!scl)
            return;
        bb_finish(d, !level);
        d->state = level ? BB_IDLE : BB_ADDR;
        d->nbits = 0;
        d->shift = 0;
        return;
    }
    if (level) {
        if ((d->state == BB_ADDR || d->state == BB_WRITE) && d->nbits < 8) {
            d->shift = (uint8_t)(d->shift << 1 | sda);
            d->nbits++;
        } else if (d->state == BB_READ_ACK) {
            d->nak = sda;
        }
        return;
    }
    switch (d->state) {
    case BB_ADDR:
        if (d->nbits < 8)
            break;
        if ((d->shift >> 1) != d->addr) {
            d->state = BB_IDLE;
            break;
        }
        d->reading = d->shift & 1;
        d->state = BB_ADDR_ACK;
        bb_hold(d, true);
        break;
    case BB_WRITE:
        if (d->nbits < 8)
            break;
        if (d->len < sizeof(d->buf))
            d->buf[d->len++] = d->shift;
        d->state = BB_WRITE_ACK;
        bb_hold(d, true);
        break;
    case BB_ADDR_ACK:
    case BB_WRITE_ACK:
        d->nbits = 0;
        d->shift = 0;
        if (d->state == BB_ADDR_ACK && d->reading) {
            memset(d->buf, 0xff, sizeof(d->buf));
            d->read(d->ctx, d->buf, sizeof(d->buf), true);
            d->pos = 0;
            d->state = BB_READ;
            bb_send_bit(d);
        } else {
            d->state = BB_WRITE;
            bb_hold(d, false);
        }
        break;
    case BB_READ:
        if (++d->nbits < 8) {
            bb_send_bit(d);
            break;
        }
        d->state = BB_READ_ACK;
        bb_hold(d, false);
        break;
    case BB_READ_ACK:
        if (d->nak) {
            d->state = BB_IDLE;
            break;
        }
        d->pos++;
        d->nbits = 0;
        d->state = BB_READ;
        bb_send_bit(d);
        break;
    default:
        break;
    }
}

void devices_gpio_changed(chip_t *m, unsigned pin, bool level)
{
    for (picocalc_sim_i2c_device_t *d = i2c_devs; d; d = d->next)
        if (pin == d->sda_pin || pin == d->scl_pin)
            i2c_pin_changed(m, d, pin, level);
    for (picocalc_sim_spi_device_t *d = spi_devs; d; d = d->next) {
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
    for (picocalc_sim_spi_device_t *d = spi_devs; d; d = d->next) {
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
    for (picocalc_sim_i2c_device_t *d = i2c_devs; d; d = d->next) {
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
    picocalc_sim_i2c_device_t *d = handle;
    d->write(d->ctx, buf, (size_t)len, nostop);
}

int devices_i2c_read(void *handle, uint8_t *buf, int len, bool nostop)
{
    picocalc_sim_i2c_device_t *d = handle;
    return d->read(d->ctx, buf, (size_t)len, nostop);
}

void devices_uart_tx(chip_t *m, int idx, uint8_t byte)
{
    (void)m;
    picocalc_sim_console_out(idx, byte);
}
