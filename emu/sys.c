#include <stdlib.h>
#include <string.h>

#include "chip.h"

// Clocks, resets, power-on state machine, oscillators, PLLs, tick generators and the small
// register blocks that the SDK's runtime init and the boot path touch.

#define XOSC_HZ 12000000ull
#define ROSC_HZ 11000000ull

static uint64_t pll_hz(chip_t *m, const pll_t *p)
{
    if (!m->xosc_hz)
        return 0;
    if (p->cs & (1u << 8))
        return m->xosc_hz;
    if (p->pwr & 0x21)
        return 0;
    uint32_t refdiv = p->cs & 0x3f;
    uint32_t pd1 = (p->prim >> 16) & 7, pd2 = (p->prim >> 12) & 7;
    if (!refdiv || !p->fbdiv || !pd1 || !pd2)
        return 0;
    return m->xosc_hz / refdiv * (p->fbdiv & 0xfff) / (pd1 * pd2);
}

static uint64_t div_int(uint64_t hz, uint32_t div, uint32_t mask)
{
    uint32_t d = (div & mask) >> 16;
    if (!d)
        d = (mask >> 16) + 1;
    return hz / d;
}

static uint64_t aux_usb_like(chip_t *m, uint32_t ctrl)
{
    switch ((ctrl >> 5) & 7) {
    case 0: return m->pll_usb_hz;
    case 1: return m->pll_sys_hz;
    case 2: return m->rosc_hz;
    case 3: return m->xosc_hz;
    default: return 0;
    }
}

void chip_update_clocks(chip_t *m)
{
    bool xosc_on = ((m->xosc_ctrl >> 12) & 0xfff) == 0xfab;
    m->xosc_hz = xosc_on ? XOSC_HZ : 0;
    m->rosc_hz = ((m->rosc_regs[0] >> 12) & 0xfff) == 0xd1e ? 0 : ROSC_HZ;
    m->pll_sys_hz = pll_hz(m, &m->pll_sys);
    m->pll_usb_hz = pll_hz(m, &m->pll_usb);
    if (m->rp2040) {
        rp2040_update_clocks(m);
        timer_retime(m);
        watchdog_retime(m);
        usb_clocks_changed(m);
        return;
    }

    uint32_t ref_ctrl = m->clk[CLK_REF].ctrl;
    uint64_t ref;
    switch (ref_ctrl & 3) {
    case 0: ref = m->rosc_hz; break;
    case 1: ref = ((ref_ctrl >> 5) & 3) == 0 ? m->pll_usb_hz : 0; break;
    case 2: ref = m->xosc_hz; break;
    default: ref = 32768; break;
    }
    m->ref_hz = div_int(ref, m->clk[CLK_REF].div, 0x00ff0000u);

    uint32_t sys_ctrl = m->clk[CLK_SYS].ctrl;
    uint64_t sys;
    if (!(sys_ctrl & 1)) {
        sys = m->ref_hz;
    } else {
        switch ((sys_ctrl >> 5) & 7) {
        case 0: sys = m->pll_sys_hz; break;
        case 1: sys = m->pll_usb_hz; break;
        case 2: sys = m->rosc_hz; break;
        case 3: sys = m->xosc_hz; break;
        default: sys = 0; break;
        }
    }
    uint32_t div = m->clk[CLK_SYS].div;
    if (!div)
        div = 1u << 16;
    sys = sys * 65536 / div;
    if (!sys)
        sys = ROSC_HZ;
    m->sys_hz = sys;
    m->sys_ps = (uint32_t)((PS_PER_S + sys / 2) / sys);
    // A core running a predecoded block keeps the old period until it looks again.
    m->core[0].check_exc = m->core[1].check_exc = true;

    uint32_t peri_ctrl = m->clk[CLK_PERI].ctrl;
    uint64_t peri = 0;
    if (peri_ctrl & (1u << 11)) {
        switch ((peri_ctrl >> 5) & 7) {
        case 0: peri = m->sys_hz; break;
        case 1: peri = m->pll_sys_hz; break;
        case 2: peri = m->pll_usb_hz; break;
        case 3: peri = m->rosc_hz; break;
        case 4: peri = m->xosc_hz; break;
        default: peri = 0; break;
        }
        peri = div_int(peri, m->clk[CLK_PERI].div, 0x00030000u);
    }
    m->peri_hz = peri;
    m->usb_hz = (m->clk[CLK_USB].ctrl & (1u << 11)) ? div_int(aux_usb_like(m, m->clk[CLK_USB].ctrl), m->clk[CLK_USB].div, 0x000f0000u) : 0;
    m->adc_hz = (m->clk[CLK_ADC].ctrl & (1u << 11)) ? div_int(aux_usb_like(m, m->clk[CLK_ADC].ctrl), m->clk[CLK_ADC].div, 0x000f0000u) : 0;
    timer_retime(m);
    watchdog_retime(m);
    usb_clocks_changed(m);
}

static uint64_t fc0_source_hz(chip_t *m, uint32_t src)
{
    switch (src) {
    case 1: return m->pll_sys_hz;
    case 2: return m->pll_usb_hz;
    case 3:
    case 4: return m->rosc_hz;
    case 5: return m->xosc_hz;
    case 8: return m->ref_hz;
    case 9: return m->sys_hz;
    case 0xa: return m->peri_hz;
    case 0xb: return m->usb_hz;
    case 0xc: return m->adc_hz;
    default: return 0;
    }
}

void chip_core1_power(chip_t *m, bool off)
{
    cpu_t *c = &m->core[1];
    if (off) {
        c->state = CPU_OFF;
        c->check_exc = false;
    } else if (c->state == CPU_OFF) {
        c->t = m->now;
        cpu_reset(c);
        if (!m->rp2040 && !m->real_rom)
            rom_core_start(c);
    }
}

static void reset_peripherals(chip_t *m, uint32_t newly)
{
    if (newly & (1u << 2))
        dma_reset(&m->dma);
    if (newly & (1u << 4))
        dw_i2c_reset(&m->i2c[0], 0);
    if (newly & (1u << 5))
        dw_i2c_reset(&m->i2c[1], 1);
    if (newly & (1u << 6)) {
        for (int i = 0; i < NUM_GPIO_MAX; i++)
            m->gpio[i].ctrl = FUNC_NULL;
        m->gpio_in_valid = false;
        for (int i = 0; i < 3; i++)
            m->pio[i].pin_owned = 0;
        memset(m->gpio_inte, 0, sizeof(m->gpio_inte));
        memset(m->gpio_intf, 0, sizeof(m->gpio_intf));
        memset(m->gpio_edge, 0, sizeof(m->gpio_edge));
        m->gpio_irq_pins = 0;
        gpio_refresh(m);
    }
    for (int i = 0; i < 3; i++)
        if (newly & (1u << (11 + i)))
            pio_reset(&m->pio[i], i);
    if (newly & (1u << 14))
        m->pll_sys = (pll_t){.cs = 1, .pwr = 0x2d, .fbdiv = 0, .prim = 0x77000};
    if (newly & (1u << 15))
        m->pll_usb = (pll_t){.cs = 1, .pwr = 0x2d, .fbdiv = 0, .prim = 0x77000};
    if (newly & (1u << 16))
        pwm_reset(&m->pwm);
    if ((newly & (1u << 17)) && !m->rp2040)
        sha256_reset(m);
    if (newly & (1u << 18))
        pl022_reset(&m->spi[0], 0);
    if (newly & (1u << 19))
        pl022_reset(&m->spi[1], 1);
    if (newly & (1u << 23))
        timer_reset(m, &m->timer[0], 0);
    if (newly & (1u << 24))
        timer_reset(m, &m->timer[1], 1);
    if (newly & (1u << 26))
        pl011_reset(&m->uart[0], 0);
    if (newly & (1u << 27))
        pl011_reset(&m->uart[1], 1);
    if (newly & (1u << 28))
        usb_reset(m);
    if (newly & (3u << 14))
        chip_update_clocks(m);
}

static uint32_t adc_sample(chip_t *m, unsigned ch)
{
    (void)m;
    // 3.3 V full scale over 12 bits. ADC3 sees VSYS/3 on a Pico 2; ADC4 is the temperature
    // sensor at about 27 C.
    switch (ch) {
    case 3: return (uint32_t)(5.0 / 3.0 / 3.3 * 4096);
    case 4: return (uint32_t)(0.706 / 3.3 * 4096);
    default: return 0;
    }
}

static uint32_t rng32(chip_t *m)
{
    uint64_t s = m->rng;
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    m->rng = s;
    return (uint32_t)(s >> 16);
}

uint32_t sysctl_read(chip_t *m, uint32_t base, uint32_t off)
{
    switch (base) {
    case 0x40000000:
        switch (off) {
        case 0x00: return (m->a3 ? 0x30000000u : 0x20000000u) | 0x00004927u;
        case 0x04: return m->board->num_gpio > 30 ? 0 : 1;
        case 0x08: return 2;
        case 0x14: return 0x8e9b3f2au;
        default: return 0;
        }
    case 0x40008000:
        return off < 0x20 ? m->syscfg[off >> 2] : 0;
    case 0x40010000:
        if (off < 0x78) {
            unsigned i = off / 12, r = off % 12;
            uint32_t ctrl = m->clk[i].ctrl;
            if (r == 0) {
                if (i != CLK_REF && i != CLK_SYS && (ctrl & (1u << 11)))
                    ctrl |= 1u << 28;
                return ctrl;
            }
            if (r == 4)
                return m->clk[i].div;
            if (i == CLK_REF)
                return 1u << (ctrl & 3);
            if (i == CLK_SYS)
                return 1u << (ctrl & 1);
            return 1;
        }
        switch (off) {
        case 0x84: return m->clk_resus;
        case 0x88: return 0;
        case 0x8c:
        case 0x90:
        case 0x94:
        case 0x98:
        case 0x9c:
        case 0xa0: return m->fc0[(off - 0x8c) >> 2];
        case 0xa4: return 0x10;
        case 0xa8: return (uint32_t)((fc0_source_hz(m, m->fc0[5] & 0xff) / 1000) << 5);
        case 0xac: return m->clk_wake[0];
        case 0xb0: return m->clk_wake[1];
        case 0xb4: return m->clk_sleep[0];
        case 0xb8: return m->clk_sleep[1];
        case 0xbc:
        case 0xc0: return 0xffffffffu;
        case 0xc8: return m->clk_inte;
        case 0xcc: return m->clk_intf;
        default: return 0;
        }
    case 0x40018000:
        switch (off) {
        case 0x0: return m->psm_frce_on;
        case 0x4: return m->psm_frce_off;
        case 0x8: return m->psm_wdsel;
        case 0xc: return 0x01ffffffu & ~m->psm_frce_off;
        default: return 0;
        }
    case 0x40020000: {
        uint32_t mask = access_ns(m) && !m->rp2040 ? accessctrl_resets_ns_mask(m, access_priv(m)) : ~0u;
        switch (off) {
        case 0x0: return m->resets & mask;
        case 0x4: return 0;
        case 0x8: return ~m->resets & 0x1fffffffu & mask;
        default: return 0;
        }
    }
    case 0x40048000:
        switch (off) {
        case 0x00: return m->xosc_ctrl;
        case 0x04: {
            bool en = ((m->xosc_ctrl >> 12) & 0xfff) == 0xfab;
            return (en ? (1u << 31) | (1u << 12) : 0) | (m->xosc_ctrl & 3);
        }
        case 0x0c: return m->xosc_startup;
        case 0x10: {
            uint64_t elapsed = (m->now - m->xosc_count_t) / (PS_PER_S / XOSC_HZ);
            return elapsed >= m->xosc_count ? 0 : (uint32_t)(m->xosc_count - elapsed);
        }
        default: return 0;
        }
    case 0x40050000:
    case 0x40058000: {
        pll_t *p = base == 0x40050000 ? &m->pll_sys : &m->pll_usb;
        switch (off) {
        case 0x00: return p->cs | ((p->pwr & 0x21) ? 0 : (1u << 31));
        case 0x04: return p->pwr;
        case 0x08: return p->fbdiv;
        case 0x0c: return p->prim;
        case 0x10: return p->intr;
        case 0x14: return p->inte;
        case 0x18: return p->intf;
        default: return 0;
        }
    }
    case 0x40060000:
        return accessctrl_read(m, off);
    case 0x40068000:
        if (off == 0x04)
            return 1;
        return off < 0x30 ? m->busctrl[off >> 2] : 0;
    case 0x400a0000:
        switch (off) {
        case 0x00: return m->adc_cs | ((m->adc_cs & 1) ? (1u << 8) : 0);
        case 0x04: return m->adc_result;
        case 0x08: return m->adc_fcs | (1u << 8);
        case 0x0c: return m->adc_result;
        case 0x10: return m->adc_div;
        case 0x18: return m->adc_inte;
        case 0x1c: return m->adc_intf;
        default: return 0;
        }
    case 0x400c0000:
        return off < 0x40 ? m->hstx_regs[off >> 2] : 0;
    case 0x400e0000:
        switch (off) {
        case 0x800: return m->bootram_write_once[0];
        case 0x804: return m->bootram_write_once[1];
        case 0x808: return m->bootlock_stat;
        default:
            if (off >= 0x80c && off < 0x82c) {
                unsigned n = (off - 0x80c) >> 2;
                if (m->bootlock_stat & (1u << n)) {
                    m->bootlock_stat &= ~(1u << n);
                    return 1u << n;
                }
                return 0;
            }
            return 0;
        }
    case 0x400e8000:
        switch (off) {
        case 0x0c: return rng32(m);
        case 0x1c: return (1u << 31) | (1u << 12);
        case 0x20: return rng32(m) & 1;
        default: return off < 0x28 ? m->rosc_regs[off >> 2] : 0;
        }
    case 0x400f0000:
        switch (off) {
        case 0x104: return 1;
        case 0x110: return 1;
        case 0x114:
        case 0x118:
        case 0x11c:
        case 0x120:
        case 0x124:
        case 0x128: return rng32(m);
        case 0x12c: return m->trng_regs[0];
        case 0x10c: return m->trng_regs[1];
        case 0x1c0: return 0x0000b5;
        default: return 0;
        }
    case 0x400f8000:
        return sha256_read(m, off);
    case 0x40108000:
        if (off < 0x48) {
            unsigned r = (off % 12) / 4, i = off / 12;
            if (r == 0)
                return m->ticks[i * 3] | ((m->ticks[i * 3] & 1) ? 2 : 0);
            return m->ticks[i * 3 + r];
        }
        return 0;
    case 0x40120000:
        switch (off) {
        case 0x124: return 0;
        case 0x15c: return 0;
        default: return off < 0x180 ? m->otp_regs[off >> 2] : 0;
        }
    case 0x40158000:
        return off < 0x40 ? m->glitch[off >> 2] : 0;
    case 0x40160000:
        return off == 0 ? 2 : 0;
    default:
        return 0;
    }
}

void sysctl_write(chip_t *m, uint32_t base, uint32_t off, uint32_t v)
{
    switch (base) {
    case 0x40008000:
        if (off < 0x20)
            m->syscfg[off >> 2] = v;
        break;
    case 0x40010000:
        if (off < 0x78) {
            unsigned i = off / 12, r = off % 12;
            if (r == 0)
                m->clk[i].ctrl = v & ~(1u << 28);
            else if (r == 4)
                m->clk[i].div = v;
            chip_update_clocks(m);
            break;
        }
        switch (off) {
        case 0x84: m->clk_resus = v; break;
        case 0x8c:
        case 0x90:
        case 0x94:
        case 0x98:
        case 0x9c:
        case 0xa0: m->fc0[(off - 0x8c) >> 2] = v; break;
        case 0xac: m->clk_wake[0] = v; break;
        case 0xb0: m->clk_wake[1] = v; break;
        case 0xb4: m->clk_sleep[0] = v; break;
        case 0xb8: m->clk_sleep[1] = v; break;
        case 0xc8: m->clk_inte = v; break;
        case 0xcc: m->clk_intf = v; break;
        default: break;
        }
        break;
    case 0x40018000:
        switch (off) {
        case 0x0: m->psm_frce_on = v & 0x1ffffff; break;
        case 0x4:
            m->psm_frce_off = v & 0x1ffffff;
            chip_core1_power(m, v & (1u << 24));
            break;
        case 0x8: m->psm_wdsel = v & 0x1ffffff; break;
        default: break;
        }
        break;
    case 0x40020000:
        if (off == 0x0) {
            // Non-secure code reaches only the resets of the peripherals it can reach.
            if (access_ns(m) && !m->rp2040) {
                uint32_t mask = accessctrl_resets_ns_mask(m, access_priv(m));
                v = (m->resets & ~mask) | (v & mask);
            }
            uint32_t newly = v & ~m->resets & 0x1fffffffu;
            m->resets = v & 0x1fffffffu;
            reset_peripherals(m, newly);
        }
        break;
    case 0x40048000:
        switch (off) {
        case 0x00:
            m->xosc_ctrl = v;
            chip_update_clocks(m);
            break;
        case 0x0c: m->xosc_startup = v; break;
        case 0x10:
            m->xosc_count = v & 0xffff;
            m->xosc_count_t = m->now;
            break;
        default: break;
        }
        break;
    case 0x40050000:
    case 0x40058000: {
        pll_t *p = base == 0x40050000 ? &m->pll_sys : &m->pll_usb;
        switch (off) {
        case 0x00: p->cs = v & 0x4000013fu; break;
        case 0x04: p->pwr = v & 0x2d; break;
        case 0x08: p->fbdiv = v & 0xfff; break;
        case 0x0c: p->prim = v & 0x77000; break;
        case 0x14: p->inte = v; break;
        case 0x18: p->intf = v; break;
        default: break;
        }
        chip_update_clocks(m);
        break;
    }
    case 0x40068000:
        if (off < 0x30)
            m->busctrl[off >> 2] = v;
        break;
    case 0x400a0000:
        switch (off) {
        case 0x00:
            m->adc_cs = v & ~(1u << 2);
            if (v & (1u << 2))
                m->adc_result = adc_sample(m, (v >> 12) & 0xf);
            break;
        case 0x08: m->adc_fcs = v; break;
        case 0x10: m->adc_div = v; break;
        case 0x18: m->adc_inte = v; break;
        case 0x1c: m->adc_intf = v; break;
        default: break;
        }
        break;
    case 0x400c0000:
        if (off < 0x40)
            m->hstx_regs[off >> 2] = v;
        break;
    case 0x400e0000:
        if (off == 0x800)
            m->bootram_write_once[0] |= v;
        else if (off == 0x804)
            m->bootram_write_once[1] |= v;
        else if (off >= 0x80c && off < 0x82c)
            m->bootlock_stat |= 1u << ((off - 0x80c) >> 2);
        break;
    case 0x400e8000:
        if (off < 0x28)
            m->rosc_regs[off >> 2] = v;
        chip_update_clocks(m);
        break;
    case 0x400f0000:
        if (off == 0x12c)
            m->trng_regs[0] = v;
        else if (off == 0x10c)
            m->trng_regs[1] = v;
        break;
    case 0x400f8000:
        sha256_write(m, off, v);
        break;
    case 0x40108000:
        if (off < 0x48) {
            unsigned r = (off % 12) / 4, i = off / 12;
            if (r < 2)
                m->ticks[i * 3 + r] = v;
            timer_retime(m);
            watchdog_retime(m);
        }
        break;
    case 0x40120000:
        if (off < 0x180)
            m->otp_regs[off >> 2] = v;
        break;
    case 0x40158000:
        if (off < 0x40)
            m->glitch[off >> 2] = v;
        break;
    default:
        break;
    }
}
