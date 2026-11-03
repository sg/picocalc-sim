#include <string.h>

#include "chip.h"

// IO_BANK0 function select and overrides, the pads, and pin-level edge and level interrupts. Pin
// levels are recomputed whenever something that drives them changes, and devices watching a pin
// hear about each transition.

#define PAD_PDE (1u << 2)
#define PAD_PUE (1u << 3)
#define PAD_IE (1u << 6)
#define PAD_OD (1u << 7)
#define PAD_ISO (1u << 8)

void gpio_reset(chip_t *m)
{
    for (int i = 0; i < NUM_GPIO_MAX; i++) {
        bool ext = m->gpio[i].ext_driven, lvl = m->gpio[i].ext_level;
        m->gpio[i] = (gpio_pin_t){.ctrl = FUNC_NULL, .ext_driven = ext, .ext_level = lvl};
        m->pads[i] = PAD_ISO | PAD_IE | PAD_PDE | (1u << 4) | 2;
    }
    for (int i = 0; i < 6; i++)
        m->qspi_ctrl[i] = 0;
    for (int i = 0; i < 3; i++)
        m->pio[i].pin_owned = 0;
    memset(m->gpio_inte, 0, sizeof(m->gpio_inte));
    memset(m->gpio_intf, 0, sizeof(m->gpio_intf));
    memset(m->gpio_edge, 0, sizeof(m->gpio_edge));
    memset(m->pads_qspi, 0, sizeof(m->pads_qspi));
    m->gpio_irq_pins = 0;
    for (int i = 0; i < NUM_GPIO_MAX; i++)
        m->gpio[i].level = m->gpio[i].ext_driven ? m->gpio[i].ext_level : false;
    m->gpio_in_valid = false;
    pinwave_update(m, g_wave_pins);
}

int gpio_func(chip_t *m, unsigned pin)
{
    return (int)(m->gpio[pin].ctrl & 0x1f);
}

static bool periph_out(chip_t *m, unsigned pin, bool *oe)
{
    int f = gpio_func(m, pin);
    switch (f) {
    case FUNC_SIO:
        *oe = (m->sio_oe[pin >> 5] >> (pin & 31)) & 1;
        return (m->sio_out[pin >> 5] >> (pin & 31)) & 1;
    case FUNC_SPI: {
        unsigned role = pin & 3;
        if (role == 0) {
            *oe = false;
            return false;
        }
        *oe = true;
        if (role == 1)
            return !pl022_cs_low(m, (int)((pin >> 3) & 1));
        return role == 3;
    }
    case FUNC_UART:
    case FUNC_UART_AUX:
        *oe = (pin & 3) == 0;
        return true;
    case FUNC_PIO0:
    case FUNC_PIO1:
    case FUNC_PIO2: {
        bool level = false;
        *oe = pio_drives(m, f - FUNC_PIO0, pin, &level);
        return level;
    }
    case FUNC_XIP_CS1:
        *oe = true;
        return !m->qmi.cs[1];
    default:
        *oe = false;
        return false;
    }
}

static bool compute_level(chip_t *m, unsigned pin)
{
    gpio_pin_t *g = &m->gpio[pin];
    uint32_t pad = m->pads[pin];
    bool oe;
    bool out = periph_out(m, pin, &oe);
    switch ((g->ctrl >> 12) & 3) {
    case 1: out = !out; break;
    case 2: out = false; break;
    case 3: out = true; break;
    default: break;
    }
    switch ((g->ctrl >> 14) & 3) {
    case 1: oe = !oe; break;
    case 2: oe = false; break;
    case 3: oe = true; break;
    default: break;
    }
    if (pad & PAD_ISO)
        return g->level;
    if (oe && !(pad & PAD_OD))
        return out;
    if (g->ext_driven)
        return g->ext_level;
    if (pad & PAD_PUE)
        return true;
    if (pad & PAD_PDE)
        return false;
    return g->level;
}

static void irq_update(chip_t *m)
{
    for (int core = 0; core < 2; core++) {
        bool any = false;
        for (int r = 0; r < 6 && !any; r++) {
            uint32_t raw = m->gpio_edge[r];
            for (int k = 0; k < 8; k++) {
                unsigned pin = (unsigned)(r * 8 + k);
                if (pin >= NUM_GPIO_MAX)
                    break;
                bool lvl = m->gpio[pin].level;
                raw |= (lvl ? 2u : 1u) << (4 * k);
            }
            if ((raw & m->gpio_inte[core][r]) | m->gpio_intf[core][r])
                any = true;
        }
        chip_irq_core(m, core, IRQ_IO_BANK0, any);
    }
}

// Pins with any interrupt enabled or forced on either core; others cannot change an IRQ line.
static uint64_t irq_pins(chip_t *m)
{
    uint64_t pins = 0;
    for (int core = 0; core < 2; core++)
        for (int r = 0; r < 6; r++) {
            uint32_t e = m->gpio_inte[core][r] | m->gpio_intf[core][r];
            for (int k = 0; k < 8 && e; k++, e >>= 4)
                if (e & 0xf)
                    pins |= 1ull << (r * 8 + k);
        }
    return pins;
}

static bool irq_enabled(chip_t *m)
{
    return m->gpio_irq_pins != 0;
}

void gpio_refresh_mask(chip_t *m, uint64_t mask)
{
    unsigned n = m->board->num_gpio;
    uint64_t changed_pins = 0;
    mask &= n >= 64 ? ~0ull : ((1ull << n) - 1);
    uint64_t waves = mask & g_wave_pins;
    while (mask) {
        unsigned pin = (unsigned)__builtin_ctzll(mask);
        mask &= mask - 1;
        bool lvl = compute_level(m, pin);
        gpio_pin_t *g = &m->gpio[pin];
        if (lvl == g->level)
            continue;
        g->level = lvl;
        m->gpio_in_valid = false;
        changed_pins |= 1ull << pin;
        m->gpio_edge[pin >> 3] |= (lvl ? 8u : 4u) << (4 * (pin & 7));
        devices_gpio_changed(m, pin, lvl);
    }
    if (changed_pins & m->gpio_irq_pins)
        irq_update(m);
    if (waves)
        pinwave_update(m, waves);
}

void gpio_refresh(chip_t *m)
{
    pl022_sync_all(m);
    gpio_refresh_mask(m, ~0ull);
    if (irq_enabled(m))
        irq_update(m);
    else {
        chip_irq_core(m, 0, IRQ_IO_BANK0, false);
        chip_irq_core(m, 1, IRQ_IO_BANK0, false);
    }
}

// PWM pins keep the level they were refreshed with, as nothing reads them back; what they put
// out over time comes from the slice settings instead.
void gpio_wave(chip_t *m, unsigned pin, pin_wave_t *w)
{
    gpio_pin_t *g = &m->gpio[pin];
    unsigned outover = (g->ctrl >> 12) & 3, oeover = (g->ctrl >> 14) & 3;
    if (gpio_func(m, pin) == FUNC_PWM && !(m->pads[pin] & (PAD_ISO | PAD_OD)) && outover < 2 && oeover != 2) {
        pwm_wave(m, pin, w);
        w->level ^= outover == 1;
        return;
    }
    memset(w, 0, sizeof(*w));
    w->level = g->level;
}

bool gpio_in(chip_t *m, unsigned pin)
{
    if (pin >= NUM_GPIO_MAX)
        return false;
    gpio_pin_t *g = &m->gpio[pin];
    if (!(m->pads[pin] & PAD_IE))
        return false;
    bool v = g->level;
    switch ((g->ctrl >> 16) & 3) {
    case 1: return !v;
    case 2: return false;
    case 3: return true;
    default: return v;
    }
}

uint64_t gpio_in_all(chip_t *m)
{
    if (m->gpio_in_valid)
        return m->gpio_in_cache;
    uint64_t v = 0;
    for (unsigned pin = 0; pin < m->board->num_gpio; pin++)
        if (gpio_in(m, pin))
            v |= 1ull << pin;
    // The QSPI pins idle high: chip select deasserted and data lines pulled up.
    v |= 0xfull << 60 | 1ull << 59;
    m->gpio_in_cache = v;
    m->gpio_in_valid = true;
    return v;
}

uint32_t iobank_read(chip_t *m, uint32_t off)
{
    if (off < 0x180) {
        unsigned pin = off >> 3;
        if (pin >= NUM_GPIO_MAX)
            return 0;
        if (off & 4)
            return m->gpio[pin].ctrl;
        bool oe;
        bool out = periph_out(m, pin, &oe);
        bool lvl = m->gpio[pin].level;
        return (out ? 1u << 9 : 0) | (oe ? 1u << 13 : 0) | (lvl ? 1u << 17 : 0);
    }
    if (off >= 0x230 && off < 0x248) {
        unsigned r = (off - 0x230) >> 2;
        uint32_t raw = m->gpio_edge[r];
        for (int k = 0; k < 8; k++) {
            unsigned pin = r * 8 + (unsigned)k;
            if (pin < NUM_GPIO_MAX)
                raw |= (m->gpio[pin].level ? 2u : 1u) << (4 * k);
        }
        return raw;
    }
    for (int core = 0; core < 2; core++) {
        uint32_t base = 0x248 + (uint32_t)core * 0x48;
        if (off >= base && off < base + 0x18)
            return m->gpio_inte[core][(off - base) >> 2];
        if (off >= base + 0x18 && off < base + 0x30)
            return m->gpio_intf[core][(off - base - 0x18) >> 2];
        if (off >= base + 0x30 && off < base + 0x48) {
            unsigned r = (off - base - 0x30) >> 2;
            uint32_t raw = iobank_read(m, 0x230 + r * 4);
            return (raw & m->gpio_inte[core][r]) | m->gpio_intf[core][r];
        }
    }
    return 0;
}

void iobank_write(chip_t *m, uint32_t off, uint32_t v)
{
    if (off < 0x180) {
        unsigned pin = off >> 3;
        if (pin < NUM_GPIO_MAX && (off & 4)) {
            m->gpio[pin].ctrl = v & 0x3003f01fu;
            m->gpio_in_valid = false;
            for (int i = 0; i < 3; i++) {
                if ((int)(v & 0x1f) == FUNC_PIO0 + i)
                    m->pio[i].pin_owned |= 1ull << pin;
                else
                    m->pio[i].pin_owned &= ~(1ull << pin);
            }
            gpio_refresh(m);
        }
        return;
    }
    if (off >= 0x230 && off < 0x248) {
        m->gpio_edge[(off - 0x230) >> 2] &= ~(v & 0xccccccccu);
        irq_update(m);
        return;
    }
    for (int core = 0; core < 2; core++) {
        uint32_t base = 0x248 + (uint32_t)core * 0x48;
        if (off >= base && off < base + 0x18) {
            m->gpio_inte[core][(off - base) >> 2] = v;
            m->gpio_irq_pins = irq_pins(m);
            irq_update(m);
            return;
        }
        if (off >= base + 0x18 && off < base + 0x30) {
            m->gpio_intf[core][(off - base - 0x18) >> 2] = v;
            m->gpio_irq_pins = irq_pins(m);
            irq_update(m);
            return;
        }
    }
}

uint32_t ioqspi_read(chip_t *m, uint32_t off)
{
    if (off < 0x40) {
        unsigned i = off >> 3;
        if (off & 4)
            return i < 6 ? m->qspi_ctrl[i] : 0;
        return 0;
    }
    return 0;
}

void ioqspi_write(chip_t *m, uint32_t off, uint32_t v)
{
    if (off < 0x40 && (off & 4) && (off >> 3) < 6)
        m->qspi_ctrl[off >> 3] = v;
}

bool qspi_ss_override(chip_t *m, bool *level)
{
    uint32_t ov = (m->qspi_ctrl[3] >> 12) & 3;
    if (ov == 2) {
        *level = false;
        return true;
    }
    if (ov == 3) {
        *level = true;
        return true;
    }
    return false;
}

uint32_t pads_read(chip_t *m, bool qspi, uint32_t off)
{
    if (qspi)
        return off < 0x1c ? m->pads_qspi[off >> 2] : 0;
    if (off == 0)
        return 0;
    unsigned i = (off - 4) >> 2;
    return i < NUM_GPIO_MAX + 2 ? m->pads[i] : 0;
}

void pads_write(chip_t *m, bool qspi, uint32_t off, uint32_t v)
{
    if (qspi) {
        if (off < 0x1c)
            m->pads_qspi[off >> 2] = v;
        return;
    }
    if (off == 0)
        return;
    unsigned i = (off - 4) >> 2;
    if (i < NUM_GPIO_MAX + 2) {
        m->pads[i] = v & 0x1ff;
        m->gpio_in_valid = false;
        if (i < NUM_GPIO_MAX)
            gpio_refresh(m);
    }
}
