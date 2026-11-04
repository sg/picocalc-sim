#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pcsim/bus.h"
#include "pcsim/devices.h"
#include "pcsim/sim.h"

// The PicoCalc mainboard's 8 MB ESP-PSRAM64H, on ordinary GPIOs. It takes SPI (SI on SIO0, SO on
// SIO1) or, in quad commands and after Enter Quad Mode, four bits a clock; it samples on the
// rising edge of SCK and changes its outputs after the falling one.

#define SIZE (8u << 20)

// From a clock edge at its pin to its output changing at its own, as a PicoCalc showed with the
// clock pad at the default 4 mA slow slew (within the datasheet's 2 to 6 ns tACLK). Rising
// outputs are quicker, which is what makes a read sampled 4 cycles after the edge at 252 MHz fail,
// and a 12 mA fast clock pad brings them 0.8 ns sooner still.
#define T_RISE_PS 3800
#define T_RISE_SHARP_PS 200
#define T_FALL_PS 5500
#define T_HZ_PS 6000

enum { PH_CMD, PH_ADDR, PH_WAIT, PH_IN, PH_OUT, PH_IGNORE };

// Manufacturer and known-good-die bytes, then the rest of the ID a PicoCalc's part returned.
static const uint8_t id_bytes[8] = {0x0d, 0x5d, 0x53, 0x32, 0xc6, 0x81, 0x8f, 0x49};

// The datasheet's limits, which a real part may or may not tolerate; going past them is reported
// once each rather than modelled. Its 50 ns tCPH between commands is left out: rp2040-psram gives
// 3 cycles there and works.
#define MAX_03_PS 30300
#define MAX_PS 7519
#define MAX_PAGE_CROSS_PS 11905
#define MAX_CE_LOW_PS (8 * 1000000ull)
enum { W_03 = 1, W_FAST = 2, W_PAGE = 4, W_CEM = 8 };

struct esp_psram {
    unsigned cs, sck, sio[4];
    uint8_t *mem;
    bool selected, qpi, wrap32, reset_armed;
    int phase;
    uint8_t cmd;
    bool write, data_quad;
    unsigned width, bits, wait;
    uint32_t shift, addr, pos;
    uint8_t out;
    unsigned out_bits;
    bool driving;
    uint64_t t_sel, t_rise, min_period;
    bool crossed;
    unsigned warned;
};

static void warn(esp_psram_t *p, unsigned kind, const char *fmt, ...)
{
    if (p->warned & kind)
        return;
    p->warned |= kind;
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    pcsim_log("PSRAM: %s", msg);
}

static double mhz(uint64_t period_ps)
{
    return 1e6 / (double)period_ps;
}

// Checks a finished command against the datasheet's timing.
static void check(esp_psram_t *p, uint64_t t)
{
    if (t - p->t_sel > MAX_CE_LOW_PS)
        warn(p, W_CEM, "command 0x%02x held CE# low for %.1f us; the datasheet allows 8 us (tCEM), as the DRAM cannot "
             "refresh meanwhile", p->cmd, (double)(t - p->t_sel) / 1e6);
    if (p->phase == PH_CMD || p->min_period == UINT64_MAX)
        return;
    if (p->cmd == 0x03 && !p->qpi && p->min_period < MAX_03_PS)
        warn(p, W_03, "read 0x03 clocked at %.0f MHz; the datasheet allows 33 MHz for it (0x0b is the fast read)",
             mhz(p->min_period));
    if (p->min_period < MAX_PS)
        warn(p, W_FAST, "command 0x%02x clocked at %.0f MHz; the datasheet allows 133 MHz", p->cmd, mhz(p->min_period));
    if (p->crossed && p->min_period < MAX_PAGE_CROSS_PS)
        warn(p, W_PAGE, "a burst crossed a 1 KB page at %.0f MHz; the datasheet allows 84 MHz across pages",
             mhz(p->min_period));
}

static void drive(esp_psram_t *p, int i, int level, uint64_t t)
{
    uint64_t rise = T_RISE_PS - T_RISE_SHARP_PS * pcsim_gpio_pad_speed(p->sck);
    pcsim_gpio_drive_input_at(p->sio[i], level, t + (level > 0 ? rise : T_FALL_PS));
}

// SO reads high whenever the part isn't sending, though the RP2350 pulls it down (seen on a
// PicoCalc, where nothing on the board pulls it up).
static void release(esp_psram_t *p)
{
    if (!p->driving)
        return;
    p->driving = false;
    uint64_t t = pcsim_gpio_edge_time() + T_HZ_PS;
    for (int i = 0; i < 4; i++)
        pcsim_gpio_drive_input_at(p->sio[i], i == 1 ? 1 : -1, t);
}

// The outputs come on at the rising edge before the first data bit, low until that bit is sent:
// a PicoCalc showed SO falling then, and reading 0 there before a first bit of 1.
static void start_output(esp_psram_t *p)
{
    uint64_t t = pcsim_gpio_edge_time();
    p->driving = true;
    for (int i = 0; i < 4; i++)
        if (i == 1 || p->data_quad)
            drive(p, i, 0, t);
}

static uint32_t next_addr(esp_psram_t *p, uint32_t a)
{
    if (p->wrap32)
        return (a & ~31u) | ((a + 1) & 31u);
    if ((a & 1023) == 1023)
        p->crossed = true;
    return (a + 1) & (SIZE - 1);
}

static void command(esp_psram_t *p)
{
    uint8_t c = p->cmd;
    bool quad_addr = p->qpi || c == 0xeb || c == 0x38;
    p->phase = PH_IGNORE;
    p->write = false;
    p->wait = 0;
    switch (c) {
    case 0x03:
    case 0x0b:
    case 0x9f:
        if (p->qpi)
            return;
        p->wait = c == 0x0b ? 8 : 0;
        p->data_quad = false;
        break;
    case 0xeb:
        p->wait = 6;
        p->data_quad = true;
        break;
    case 0x02:
    case 0x38:
        p->write = true;
        p->data_quad = quad_addr;
        break;
    default:
        return;
    }
    p->phase = PH_ADDR;
    p->width = quad_addr ? 4 : 1;
    p->bits = 0;
    p->shift = 0;
}

static void on_cs(void *ctx, unsigned pin, bool level)
{
    esp_psram_t *p = ctx;
    (void)pin;
    bool sel = !level;
    if (sel == p->selected)
        return;
    p->selected = sel;
    uint64_t t = pcsim_gpio_edge_time();
    if (sel) {
        p->t_sel = t;
        p->t_rise = 0;
        p->min_period = UINT64_MAX;
        p->crossed = false;
        p->phase = PH_CMD;
        p->width = p->qpi ? 4 : 1;
        p->bits = 0;
        p->shift = 0;
        p->out_bits = 0;
        return;
    }
    release(p);
    check(p, t);
    // Mode commands act once CS rises after their eight bits.
    bool whole = p->phase == PH_IGNORE;
    bool armed = p->reset_armed;
    p->reset_armed = false;
    if (!whole)
        return;
    switch (p->cmd) {
    case 0x35: p->qpi = true; break;
    case 0xf5: p->qpi = false; break;
    case 0xc0: p->wrap32 = !p->wrap32; break;
    case 0x66: p->reset_armed = true; break;
    case 0x99:
        if (armed) {
            p->qpi = false;
            p->wrap32 = false;
        }
        break;
    default: break;
    }
}

static unsigned sample(esp_psram_t *p)
{
    if (p->width == 1)
        return pcsim_gpio_level(p->sio[0]);
    unsigned v = 0;
    for (int i = 3; i >= 0; i--)
        v = (v << 1) | pcsim_gpio_level(p->sio[i]);
    return v;
}

static void rising(esp_psram_t *p)
{
    uint64_t t = pcsim_gpio_edge_time();
    if (p->t_rise && t - p->t_rise < p->min_period)
        p->min_period = t - p->t_rise;
    p->t_rise = t;
    switch (p->phase) {
    case PH_CMD:
        p->shift = (p->shift << p->width) | sample(p);
        if ((p->bits += p->width) == 8) {
            p->cmd = (uint8_t)p->shift;
            command(p);
        }
        break;
    case PH_ADDR:
        p->shift = (p->shift << p->width) | sample(p);
        if ((p->bits += p->width) < 24)
            break;
        p->addr = p->shift & (SIZE - 1);
        p->pos = 0;
        p->width = p->data_quad ? 4 : 1;
        p->bits = 0;
        p->shift = 0;
        p->phase = p->write ? PH_IN : p->wait ? PH_WAIT : PH_OUT;
        if (p->phase == PH_OUT)
            start_output(p);
        break;
    case PH_WAIT:
        if (--p->wait == 0) {
            p->phase = PH_OUT;
            start_output(p);
        }
        break;
    case PH_IN:
        p->shift = (p->shift << p->width) | sample(p);
        if ((p->bits += p->width) == 8) {
            p->mem[p->addr] = (uint8_t)p->shift;
            p->addr = next_addr(p, p->addr);
            p->bits = 0;
            p->shift = 0;
        }
        break;
    default:
        break;
    }
}

static void falling(esp_psram_t *p)
{
    if (p->phase != PH_OUT)
        return;
    if (!p->out_bits) {
        if (p->cmd == 0x9f) {
            p->out = id_bytes[p->pos++ & 7];
        } else {
            p->out = p->mem[p->addr];
            p->addr = next_addr(p, p->addr);
        }
        p->out_bits = 8;
    }
    p->driving = true;
    uint64_t t = pcsim_gpio_edge_time();
    if (p->width == 1) {
        drive(p, 1, p->out >> 7, t);
    } else {
        for (int i = 0; i < 4; i++)
            drive(p, i, (p->out >> (4 + i)) & 1, t);
    }
    p->out = (uint8_t)(p->out << p->width);
    p->out_bits -= p->width;
}

static void on_sck(void *ctx, unsigned pin, bool level)
{
    esp_psram_t *p = ctx;
    (void)pin;
    if (!p->selected)
        return;
    if (level)
        rising(p);
    else
        falling(p);
}

esp_psram_t *esp_psram_create(unsigned cs_pin, unsigned sck_pin, const unsigned sio_pins[4])
{
    esp_psram_t *p = calloc(1, sizeof(*p));
    p->cs = cs_pin;
    p->sck = sck_pin;
    memcpy(p->sio, sio_pins, sizeof(p->sio));
    p->mem = malloc(SIZE);
    // DRAM cells power up holding whatever they settle to, never all zeros.
    uint32_t x = 0x9e3779b9u;
    for (uint32_t i = 0; i < SIZE; i += 4) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        memcpy(p->mem + i, &x, 4);
    }
    pcsim_gpio_watch(cs_pin, on_cs, p);
    pcsim_gpio_watch(sck_pin, on_sck, p);
    pcsim_gpio_drive_input(p->sio[1], 1);
    return p;
}
