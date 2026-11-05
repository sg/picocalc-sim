#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chip.h"
#include "picocalc-sim/sim.h"

// The RP2040 (Pico, Pico W): its own boot ROM, run as it is, and the register layout it puts
// over the same peripherals the RP2350 has. Most blocks are the RP2350's models reached through
// a translation of addresses and fields; the flash interface (an SSI with an XIP cache), the
// clock tree, reset bits, the SIO divider and the RTC are the RP2040's own.

#define ROSC_HZ 6500000ull
#define ROM_BYTES 0x4000u

// ---------------------------------------------------------------------------------------------
// Boot ROM

static bool load_elf(const uint8_t *f, size_t n, uint8_t *rom, char *err, size_t errlen)
{
    if (n < 52 || f[4] != 1 || f[5] != 1) {
        snprintf(err, errlen, "not a 32-bit little-endian ELF file");
        return false;
    }
    uint32_t phoff = rd32le(f + 28);
    unsigned phentsize = rd16le(f + 42), phnum = rd16le(f + 44);
    bool any = false;
    for (unsigned i = 0; i < phnum; i++) {
        size_t ph = phoff + (size_t)i * phentsize;
        if (ph + 32 > n)
            break;
        uint32_t type = rd32le(f + ph), off = rd32le(f + ph + 4), paddr = rd32le(f + ph + 12);
        uint32_t filesz = rd32le(f + ph + 16);
        if (type != 1 || !filesz || paddr >= ROM_BYTES)
            continue;
        if (paddr + filesz > ROM_BYTES || off + (size_t)filesz > n) {
            snprintf(err, errlen, "ELF segment does not fit the 16 KB ROM");
            return false;
        }
        memcpy(rom + paddr, f + off, filesz);
        any = true;
    }
    if (!any)
        snprintf(err, errlen, "ELF file has nothing to load at address 0");
    return any;
}

bool rp2040_load_rom(chip_t *m, const char *path, char *err, size_t errlen)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        snprintf(err, errlen, "cannot open %s", path);
        return false;
    }
    static uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    uint8_t rom[ROM_BYTES];
    memset(rom, 0, sizeof(rom));
    char why[128];
    if (n >= 4 && !memcmp(buf, "\x7f" "ELF", 4)) {
        if (!load_elf(buf, n, rom, why, sizeof(why))) {
            snprintf(err, errlen, "%s: %s", path, why);
            return false;
        }
    } else if (n == ROM_BYTES) {
        memcpy(rom, buf, ROM_BYTES);
    } else {
        snprintf(err, errlen, "%s is neither an ELF file nor a 16 KB ROM image", path);
        return false;
    }
    if (rom[0x10] != 'M' || rom[0x11] != 'u' || rom[0x12] != 1) {
        snprintf(err, errlen, "%s is not an RP2040 boot ROM", path);
        return false;
    }
    for (uint32_t i = 0; i < sizeof(m->rom); i += ROM_BYTES)
        memcpy(m->rom + i, rom, ROM_BYTES);
    if (rom[0x13] != 3)
        picocalc_sim_log("%s is the RP2040-B%d boot ROM; current Picos have B2", path, rom[0x13] - 1);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Interrupt and DMA request numbering

int rp2040_irq_line(unsigned irq, int core)
{
    switch (irq) {
    case IRQ_TIMER0_0:
    case IRQ_TIMER0_0 + 1:
    case IRQ_TIMER0_0 + 2:
    case IRQ_TIMER0_0 + 3: return (int)irq;
    case IRQ_PWM_WRAP_0: return 4;
    case IRQ_USBCTRL: return 5;
    case IRQ_PIO0_0:
    case IRQ_PIO0_0 + 1:
    case IRQ_PIO0_0 + 2:
    case IRQ_PIO0_0 + 3: return 7 + (int)(irq - IRQ_PIO0_0);
    case IRQ_DMA_0:
    case IRQ_DMA_0 + 1: return 11 + (int)(irq - IRQ_DMA_0);
    case IRQ_IO_BANK0: return 13;
    case IRQ_IO_QSPI: return 14;
    // Each core's FIFO interrupt is wired to that core's NVIC alone.
    case IRQ_SIO_FIFO: return 15 + core;
    case IRQ_CLOCKS: return 17;
    case IRQ_SPI0: return 18;
    case IRQ_SPI1: return 19;
    case IRQ_UART0: return 20;
    case IRQ_UART1: return 21;
    case IRQ_ADC_FIFO: return 22;
    case IRQ_I2C0: return 23;
    case IRQ_I2C1: return 24;
    case IRQ_RTC: return 25;
    default: return -1;
    }
}

static const uint8_t dreq_from_rp2040[40] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
    DREQ_SPI0_TX, DREQ_SPI0_RX, DREQ_SPI1_TX, DREQ_SPI1_RX,
    DREQ_UART0_TX, DREQ_UART0_RX, DREQ_UART1_TX, DREQ_UART1_RX,
    DREQ_PWM_WRAP0, DREQ_PWM_WRAP0 + 1, DREQ_PWM_WRAP0 + 2, DREQ_PWM_WRAP0 + 3,
    DREQ_PWM_WRAP0 + 4, DREQ_PWM_WRAP0 + 5, DREQ_PWM_WRAP0 + 6, DREQ_PWM_WRAP0 + 7,
    DREQ_I2C0_TX, DREQ_I2C0_RX, DREQ_I2C1_TX, DREQ_I2C1_RX, DREQ_ADC, DREQ_XIP_STREAM,
    DREQ_XIP_QMITX, DREQ_XIP_QMIRX,
};

// A DMA TREQ_SEL value from the RP2040's numbering to the one the DMA model uses.
uint32_t rp2040_dreq_in(uint32_t treq)
{
    if (treq < 40)
        return dreq_from_rp2040[treq];
    // Numbers with nothing behind them never assert; the timers and "permanent" match.
    return treq >= DREQ_DMA_TIMER0 ? treq : 58;
}

uint32_t rp2040_dreq_out(uint32_t treq)
{
    for (uint32_t i = 0; i < 40; i++)
        if (dreq_from_rp2040[i] == treq)
            return i;
    return treq >= DREQ_DMA_TIMER0 ? treq : 40;
}

// ---------------------------------------------------------------------------------------------
// Resets

// RESETS bits on the RP2040, as the bit of the same block in the RP2350 layout the models use.
static const int8_t reset_bit[25] = {0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, -1,
                                     18, 19, 20, 21, 22, 23, 26, 27, 28};

enum {
    RST_IO_BANK0 = 1u << 5,
    RST_IO_QSPI = 1u << 6,
    RST_PADS_BANK0 = 1u << 8,
    RST_PADS_QSPI = 1u << 9,
    RST_RTC = 1u << 15,
    RST_ALL = 0x01ffffffu,
};

static const uint32_t pads_qspi_reset[7] = {0, 0x56, 0x52, 0x52, 0x52, 0x52, 0x5a};

static void qspi_io_reset(chip_t *m)
{
    for (int i = 0; i < 6; i++)
        m->rp.qspi_ctrl[i] = FUNC_NULL;
}

static void pads_bank0_reset(chip_t *m)
{
    for (int i = 0; i < 30; i++)
        m->pads[i] = 0x56;
    m->pads[30] = 0xda;
    m->pads[31] = 0x5a;
    m->gpio_in_valid = false;
    gpio_refresh(m);
}

static void rtc_reset(chip_t *m)
{
    memset(m->rp.rtc, 0, sizeof(m->rp.rtc));
    m->rp.rtc_base_s = 0;
    m->rp.rtc_base_t = m->now;
}

static void ssi_reset(chip_t *m);

static void set_resets(chip_t *m, uint32_t v)
{
    v &= RST_ALL;
    uint32_t newly = v & ~m->rp.resets;
    m->rp.resets = v;
    uint32_t internal = 0;
    for (int i = 0; i < 25; i++)
        if (reset_bit[i] >= 0 && (v & (1u << i)))
            internal |= 1u << reset_bit[i];
    // The blocks the RP2350 has and the RP2040 lacks stay in reset.
    internal |= (1u << 3) | (1u << 13) | (1u << 17) | (1u << 24) | (1u << 25);
    sysctl_write(m, 0x40020000, 0, internal);
    if (newly & RST_IO_BANK0)
        for (int i = 0; i < 30; i++)
            m->rp.gpio_ctrl[i] = FUNC_NULL;
    if (newly & RST_IO_QSPI)
        qspi_io_reset(m);
    if (newly & RST_PADS_QSPI)
        memcpy(m->rp.pads_qspi, pads_qspi_reset, sizeof(pads_qspi_reset));
    if (newly & RST_PADS_BANK0)
        pads_bank0_reset(m);
    if (newly & RST_RTC)
        rtc_reset(m);
}

void rp2040_reset(chip_t *m, bool power_on)
{
    rp2040_t *r = &m->rp;
    uint32_t chip_reset = r->chip_reset;
    memset(r->gpio_ctrl, 0, sizeof(r->gpio_ctrl));
    for (int i = 0; i < 30; i++)
        r->gpio_ctrl[i] = FUNC_NULL;
    qspi_io_reset(m);
    memcpy(r->pads_qspi, pads_qspi_reset, sizeof(pads_qspi_reset));
    pads_bank0_reset(m);
    for (int i = 0; i < 10; i++) {
        m->clk[i].ctrl = 0;
        m->clk[i].div = 0x100;
    }
    r->wd_tick = 0x200;
    m->ticks[2 * 3] = m->ticks[4 * 3] = 1;
    m->ticks[2 * 3 + 1] = m->ticks[4 * 3 + 1] = 0;
    r->vreg = 0xb1;
    r->bod = 0x91;
    r->chip_reset = power_on ? 1u << 8 : (chip_reset & ~(1u << 24)) | (1u << 16);
    memset(r->syscfg, 0, sizeof(r->syscfg));
    r->syscfg[2] = 0x10000000u;
    r->xip_ctrl = 0x3;
    m->xip_ctrl = r->xip_ctrl;
    memset(r->div, 0, sizeof(r->div));
    r->sio_hi_out = r->sio_hi_oe = 0;
    memset(r->dma_ctrl, 0, sizeof(r->dma_ctrl));
    r->psm_frce_on = r->psm_frce_off = r->psm_wdsel = 0;
    memset(r->busctrl, 0, sizeof(r->busctrl));
    for (int i = 0; i < 4; i++)
        r->busctrl[3 + 2 * i] = 0x1f;
    r->xip_stream_addr = r->xip_stream_ctr = 0;
    rtc_reset(m);
    r->flash_crm = false;
    ssi_reset(m);
    r->resets = 0;
    set_resets(m, RST_ALL);
}

// ---------------------------------------------------------------------------------------------
// Clocks

enum { CK_GPOUT0, CK_REF = 4, CK_SYS, CK_PERI, CK_USB, CK_ADC, CK_RTC };

// Dividers with 24 integer and 8 fractional bits, an integer part of 0 meaning 2^24.
static uint64_t div_frac8(uint64_t hz, uint32_t div)
{
    uint64_t d = (div >> 8) ? div : ((uint64_t)1 << 32) | (div & 0xff);
    return hz * 256 / d;
}

// And 2-bit integer dividers, 0 meaning 4.
static uint64_t div_int2(uint64_t hz, uint32_t div)
{
    uint32_t d = (div >> 8) & 3;
    return hz / (d ? d : 4);
}

static uint64_t aux_src(uint32_t ctrl, const uint64_t *srcs, unsigned n)
{
    unsigned s = (ctrl >> 5) & 7;
    return s < n ? srcs[s] : 0;
}

void rp2040_update_clocks(chip_t *m)
{
    m->rosc_hz = ((m->rosc_regs[0] >> 12) & 0xfff) == 0xd1e ? 0 : ROSC_HZ;
    uint32_t ref_ctrl = m->clk[CK_REF].ctrl;
    uint64_t ref;
    switch (ref_ctrl & 3) {
    case 0: ref = m->rosc_hz; break;
    case 1: ref = ((ref_ctrl >> 5) & 3) == 0 ? m->pll_usb_hz : 0; break;
    case 2: ref = m->xosc_hz; break;
    default: ref = 0; break;
    }
    m->ref_hz = div_int2(ref, m->clk[CK_REF].div);

    uint32_t sys_ctrl = m->clk[CK_SYS].ctrl;
    uint64_t sys;
    if (!(sys_ctrl & 1)) {
        sys = m->ref_hz;
    } else {
        const uint64_t srcs[] = {m->pll_sys_hz, m->pll_usb_hz, m->rosc_hz, m->xosc_hz};
        sys = aux_src(sys_ctrl, srcs, 4);
    }
    sys = div_frac8(sys, m->clk[CK_SYS].div);
    if (!sys)
        sys = ROSC_HZ;
    m->sys_hz = sys;
    m->sys_ps = (uint32_t)((PS_PER_S + sys / 2) / sys);
    m->core[0].check_exc = m->core[1].check_exc = true;

    uint32_t c = m->clk[CK_PERI].ctrl;
    const uint64_t peri_srcs[] = {m->sys_hz, m->pll_sys_hz, m->pll_usb_hz, m->rosc_hz, m->xosc_hz};
    m->peri_hz = (c & (1u << 11)) ? aux_src(c, peri_srcs, 5) : 0;
    const uint64_t aux_srcs[] = {m->pll_usb_hz, m->pll_sys_hz, m->rosc_hz, m->xosc_hz};
    c = m->clk[CK_USB].ctrl;
    m->usb_hz = (c & (1u << 11)) ? div_int2(aux_src(c, aux_srcs, 4), m->clk[CK_USB].div) : 0;
    c = m->clk[CK_ADC].ctrl;
    m->adc_hz = (c & (1u << 11)) ? div_int2(aux_src(c, aux_srcs, 4), m->clk[CK_ADC].div) : 0;
    c = m->clk[CK_RTC].ctrl;
    m->rp.rtc_hz = (c & (1u << 11)) ? div_frac8(aux_src(c, aux_srcs, 4), m->clk[CK_RTC].div) : 0;
}

static uint64_t fc0_hz(chip_t *m, uint32_t src)
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
    case 0xd: return m->rp.rtc_hz;
    default: return 0;
    }
}

static uint32_t clocks_read(chip_t *m, uint32_t off)
{
    if (off < 0x78) {
        unsigned i = off / 12, r = off % 12;
        if (r == 0)
            return m->clk[i].ctrl;
        if (r == 4)
            return m->clk[i].div;
        if (i == CK_REF)
            return 1u << (m->clk[i].ctrl & 3);
        if (i == CK_SYS)
            return 1u << (m->clk[i].ctrl & 1);
        return 1;
    }
    switch (off) {
    case 0x78: return m->clk_resus;
    case 0x80:
    case 0x84:
    case 0x88:
    case 0x8c:
    case 0x90:
    case 0x94: return m->fc0[(off - 0x80) >> 2];
    case 0x98: return 0x10;
    case 0x9c: return (uint32_t)((fc0_hz(m, m->fc0[5] & 0xff) / 1000) << 5);
    case 0xa0: return m->clk_wake[0];
    case 0xa4: return m->clk_wake[1];
    case 0xa8: return m->clk_sleep[0];
    case 0xac: return m->clk_sleep[1];
    case 0xb0:
    case 0xb4: return 0xffffffffu;
    case 0xbc: return m->clk_inte;
    case 0xc0: return m->clk_intf;
    default: return 0;
    }
}

static void clocks_write(chip_t *m, uint32_t off, uint32_t v)
{
    if (off < 0x78) {
        unsigned i = off / 12, r = off % 12;
        if (r == 0)
            m->clk[i].ctrl = v;
        else if (r == 4)
            m->clk[i].div = v;
        chip_update_clocks(m);
        return;
    }
    switch (off) {
    case 0x78: m->clk_resus = v; break;
    case 0x80:
    case 0x84:
    case 0x88:
    case 0x8c:
    case 0x90:
    case 0x94: m->fc0[(off - 0x80) >> 2] = v; break;
    case 0xa0: m->clk_wake[0] = v; break;
    case 0xa4: m->clk_wake[1] = v; break;
    case 0xa8: m->clk_sleep[0] = v; break;
    case 0xac: m->clk_sleep[1] = v; break;
    case 0xbc: m->clk_inte = v; break;
    case 0xc0: m->clk_intf = v; break;
    default: break;
    }
}

// The watchdog's tick generator clocks both the timer and the watchdog on the RP2040.
static void wd_tick_write(chip_t *m, uint32_t v)
{
    m->rp.wd_tick = v & 0x3ff;
    for (int gen = 2; gen <= 4; gen += 2) {
        m->ticks[gen * 3] = (v >> 9) & 1;
        m->ticks[gen * 3 + 1] = v & 0x1ff;
    }
    timer_retime(m);
    watchdog_retime(m);
}

// ---------------------------------------------------------------------------------------------
// RTC

static int64_t days_from_civil(int64_t y, unsigned mo, unsigned d)
{
    y -= mo <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, unsigned *mo, unsigned *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *mo = mp < 10 ? mp + 3 : mp - 9;
    *y = (int64_t)yoe + era * 400 + (*mo <= 2);
}

static bool rtc_running(chip_t *m)
{
    return (m->rp.rtc[3] & 1) && m->rp.rtc_hz;
}

static int64_t rtc_seconds(chip_t *m)
{
    rp2040_t *r = &m->rp;
    if (!rtc_running(m))
        return r->rtc_base_s;
    uint64_t div = (uint64_t)(r->rtc[0] & 0xffff) + 1;
    uint64_t ticks = (m->now - r->rtc_base_t) / (PS_PER_S / r->rtc_hz);
    return r->rtc_base_s + (int64_t)(ticks / div);
}

static void rtc_rebase(chip_t *m)
{
    m->rp.rtc_base_s = rtc_seconds(m);
    m->rp.rtc_base_t = m->now;
}

static uint32_t rtc_read(chip_t *m, uint32_t off)
{
    rp2040_t *r = &m->rp;
    int64_t s = rtc_seconds(m);
    int64_t days = s >= 0 ? s / 86400 : (s - 86399) / 86400;
    int64_t sec = s - days * 86400;
    int64_t y;
    unsigned mo, d;
    civil_from_days(days, &y, &mo, &d);
    unsigned dotw = (unsigned)((days % 7 + 11) % 7);
    switch (off) {
    case 0x0c: return (r->rtc[3] & ~2u) | (rtc_running(m) ? 2u : 0);
    case 0x18: return ((uint32_t)y << 12) | (mo << 8) | d;
    case 0x1c: return (dotw << 24) | ((uint32_t)(sec / 3600) << 16) | ((uint32_t)(sec / 60 % 60) << 8) | (uint32_t)(sec % 60);
    default: return off < 0x30 ? r->rtc[off >> 2] : 0;
    }
}

static void rtc_write(chip_t *m, uint32_t off, uint32_t v)
{
    rp2040_t *r = &m->rp;
    if (off >= 0x30 || off == 0x18 || off == 0x1c)
        return;
    rtc_rebase(m);
    if (off == 0x0c && (v & (1u << 4))) {
        uint32_t s0 = r->rtc[1], s1 = r->rtc[2];
        int64_t days = days_from_civil((s0 >> 12) & 0xfff, (s0 >> 8) & 0xf, s0 & 0x1f);
        r->rtc_base_s = days * 86400 + ((s1 >> 16) & 0x1f) * 3600 + ((s1 >> 8) & 0x3f) * 60 + (s1 & 0x3f);
        v &= ~(1u << 4);
    }
    r->rtc[off >> 2] = v;
}

// ---------------------------------------------------------------------------------------------
// Flash: the SSI, the QSPI pins and the XIP cache controller

// What the flash's MOSI line carries when SD0's output driver is off: its pull.
static uint8_t mosi_seen(chip_t *m, uint8_t mosi)
{
    uint32_t pad = m->rp.pads_qspi[2];
    if (!(pad & 0x80))
        return mosi;
    return (pad & 0x08) ? 0xff : 0x00;
}

static unsigned ss_outover(chip_t *m)
{
    return (m->rp.qspi_ctrl[1] >> 8) & 3;
}

// Whether the flash is selected, from the SSI's own chip select and the pin's overrides.
static bool flash_selected(chip_t *m)
{
    uint32_t ctrl = m->rp.qspi_ctrl[1];
    bool high;
    if ((ctrl & 0x1f) == 0)
        high = !m->rp.ssi.auto_cs;
    else if ((ctrl & 0x1f) == FUNC_SIO)
        high = !((m->rp.sio_hi_oe >> 1) & 1) || ((m->rp.sio_hi_out >> 1) & 1);
    else
        high = true;
    switch (ss_outover(m)) {
    case 1: high = !high; break;
    case 2: high = false; break;
    case 3: high = true; break;
    default: break;
    }
    return !high;
}

static void update_select(chip_t *m)
{
    flash_dev_select(m, flash_selected(m));
}

static uint64_t ssi_clock_ps(chip_t *m)
{
    uint32_t div = m->rp.ssi.baudr & 0xfffe;
    return (uint64_t)(div ? div : 2) * m->sys_ps;
}

// Ends the SSI's own selection once what it was sent has finished shifting out.
static void ssi_settle(chip_t *m)
{
    ssi_t *s = &m->rp.ssi;
    if (s->auto_cs && m->now >= s->busy_until) {
        s->auto_cs = false;
        update_select(m);
    }
}

static void rx_push(ssi_t *s, uint32_t v)
{
    if (s->rxn == 16) {
        s->rx_over = true;
        return;
    }
    s->rxf[(s->rxh + s->rxn) & 15] = v;
    s->rxn++;
}

static unsigned lanes(unsigned frf)
{
    return frf == 2 ? 4 : frf == 1 ? 2 : 1;
}

// A read in which the flash sees an optional instruction, then address bits (24, or 32 with mode
// bits last). Gives whether it answers with data from *addr, and leaves the flash in continuous
// read mode or not as the real part would.
static bool flash_read_cmd(chip_t *m, bool has_inst, uint8_t inst, uint32_t addr_val, unsigned addr_bits,
                           unsigned addr_frf, unsigned data_frf, uint32_t *addr)
{
    rp2040_t *r = &m->rp;
    bool mode = addr_bits >= 32;
    uint32_t a = (mode ? addr_val >> (addr_bits - 24) : addr_val) & 0xffffff;
    uint8_t mbits = mode ? (uint8_t)(addr_val >> (addr_bits - 32)) : 0;
    if (r->flash_crm) {
        if (has_inst || addr_frf != 2 || data_frf != 2) {
            r->flash_crm = false;
            return false;
        }
        r->flash_crm = (mbits & 0x30) == 0x20;
        *addr = a;
        return true;
    }
    if (!has_inst)
        return false;
    bool quad = false;
    switch (inst) {
    case 0x03:
    case 0x0b:
        if (addr_frf || data_frf)
            return false;
        break;
    case 0x3b:
        if (addr_frf || data_frf != 1)
            return false;
        break;
    case 0xbb:
        if (addr_frf != 1 || data_frf != 1)
            return false;
        break;
    case 0x6b:
        if (addr_frf || data_frf != 2)
            return false;
        quad = true;
        break;
    case 0xeb:
        if (addr_frf != 2 || data_frf != 2 || !mode)
            return false;
        quad = true;
        r->flash_crm = (mbits & 0x30) == 0x20;
        break;
    default: return false;
    }
    if (quad && !flash_dev_quad_enabled(m)) {
        r->flash_crm = false;
        return false;
    }
    *addr = a;
    return true;
}

static uint32_t flash_be32(chip_t *m, uint32_t addr)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v = (v << 8) | m->flash[(addr + (uint32_t)i) & (m->flash_size - 1)];
    return v;
}

static void ssi_eval_xip(chip_t *m);

// The SSI's enhanced-mode read with what was written to DR as instruction and address.
static void ssi_enhanced_read(chip_t *m)
{
    ssi_t *s = &m->rp.ssi;
    unsigned frf = (s->ctrlr0 >> 21) & 3, tt = s->spi_ctrlr0 & 3;
    unsigned inst_l = (s->spi_ctrlr0 >> 8) & 3, addr_l = (s->spi_ctrlr0 >> 2) & 15;
    unsigned wait = (s->spi_ctrlr0 >> 11) & 31, dfs = ((s->ctrlr0 >> 16) & 31) + 1;
    int k = 0;
    uint8_t inst = inst_l ? (uint8_t)s->eeprom_tx[k++] : 0;
    uint32_t addr_val = addr_l ? s->eeprom_tx[k] : 0;
    unsigned addr_bits = addr_l * 4, addr_frf = tt ? frf : 0;
    unsigned frames = (s->ctrlr1 & 0xffff) + 1;
    flash_dev_select(m, false);
    uint32_t addr;
    bool ok = flash_read_cmd(m, inst_l != 0, inst, addr_val, addr_bits, addr_frf, frf, &addr);
    for (unsigned i = 0; i < frames; i++) {
        uint32_t v = ok ? flash_be32(m, addr + i * (dfs / 8)) >> (32 - dfs) : 0xffffffffu >> (32 - dfs);
        rx_push(s, v);
    }
    unsigned clocks = (inst_l ? 8u / lanes(tt == 2 ? frf : 0) : 0) + addr_bits / lanes(addr_frf) + wait +
                      frames * dfs / lanes(frf);
    s->busy_until = m->now + clocks * ssi_clock_ps(m);
    s->eeprom_n = 0;
    ssi_eval_xip(m);
}

// A frame written to DR.
static void ssi_dr_write(chip_t *m, uint32_t v)
{
    ssi_t *s = &m->rp.ssi;
    if (!(s->ssienr & 1))
        return;
    unsigned tmod = (s->ctrlr0 >> 8) & 3, frf = (s->ctrlr0 >> 21) & 3;
    unsigned dfs = ((s->ctrlr0 >> 16) & 31) + 1;
    if (frf && (tmod == 2 || tmod == 3)) {
        unsigned need = (((s->spi_ctrlr0 >> 8) & 3) ? 1 : 0) + (((s->spi_ctrlr0 >> 2) & 15) ? 1 : 0);
        if (s->eeprom_n < 2)
            s->eeprom_tx[s->eeprom_n++] = v;
        if (s->eeprom_n >= (int)(need ? need : 1))
            ssi_enhanced_read(m);
        return;
    }
    ssi_settle(m);
    if (!s->auto_cs) {
        s->auto_cs = true;
        s->busy_until = m->now;
    }
    update_select(m);
    bool sel = flash_selected(m);
    uint32_t rx = 0;
    unsigned nbytes = (dfs + 7) / 8;
    for (unsigned b = 0; b < nbytes; b++) {
        uint8_t mosi = (uint8_t)(v >> (8 * (nbytes - 1 - b)));
        uint8_t miso = sel ? flash_dev_xfer(m, mosi_seen(m, mosi)) : 0xff;
        rx = (rx << 8) | miso;
    }
    if (dfs < 32)
        rx &= (1u << dfs) - 1;
    if (s->busy_until < m->now)
        s->busy_until = m->now;
    s->busy_until += dfs * ssi_clock_ps(m);
    if (tmod != 1)
        rx_push(s, rx);
    // An EEPROM read in standard mode sends what was written, then reads NDF+1 frames.
    if (tmod == 3) {
        unsigned frames = (s->ctrlr1 & 0xffff) + 1;
        for (unsigned i = 0; i < frames; i++) {
            uint32_t w = 0;
            for (unsigned b = 0; b < nbytes; b++)
                w = (w << 8) | (sel ? flash_dev_xfer(m, 0) : 0xff);
            rx_push(s, w);
        }
        s->busy_until += frames * dfs * ssi_clock_ps(m);
    }
}

static void ssi_reset(chip_t *m)
{
    ssi_t *s = &m->rp.ssi;
    memset(s, 0, sizeof(*s));
    s->ctrlr0 = 0x00070000u;
    s->baudr = 0;
    s->spi_ctrlr0 = 0x03000218u;
    s->txftlr = s->rxftlr = 0;
    s->imr = 0x3f;
    flash_dev_select(m, false);
    ssi_eval_xip(m);
}

// Works out whether XIP reads get flash data with the SSI and flash as they are, and what a cache
// line costs; the pages are mapped straight onto the flash only while they do.
static void ssi_eval_xip(chip_t *m)
{
    ssi_t *s = &m->rp.ssi;
    rp2040_t *r = &m->rp;
    bool was = s->xip_ok;
    bool ok = (s->ssienr & 1) && ((s->ctrlr0 >> 8) & 3) == 3 && (r->qspi_ctrl[1] & 0x1f) == 0 && ss_outover(m) == 0;
    unsigned frf = (s->ctrlr0 >> 21) & 3, tt = s->spi_ctrlr0 & 3;
    unsigned inst_l = (s->spi_ctrlr0 >> 8) & 3, addr_l = (s->spi_ctrlr0 >> 2) & 15;
    unsigned wait = (s->spi_ctrlr0 >> 11) & 31;
    uint8_t cmd = (uint8_t)(s->spi_ctrlr0 >> 24);
    unsigned addr_frf = tt ? frf : 0;
    if (ok) {
        bool crm = r->flash_crm;
        uint32_t addr;
        // With no instruction, XIP_CMD follows the address as mode bits.
        uint32_t addr_val = inst_l ? 0 : cmd;
        ok = flash_read_cmd(m, inst_l != 0, cmd, addr_val, addr_l * 4, addr_frf, frf, &addr);
        // A setting that only works once (an instruction that puts the flash into continuous
        // mode, after which it would be taken as address) is left to the slow path.
        if (ok && crm != r->flash_crm && inst_l)
            ok = false;
        r->flash_crm = crm;
    }
    unsigned clocks = (inst_l ? 8u / lanes(tt == 2 ? frf : 0) : 0) + addr_l * 4 / lanes(addr_frf) + wait + 64 / lanes(frf);
    uint32_t div = s->baudr & 0xfffe;
    s->miss_cyc = 4 + clocks * (div ? div : 2);
    s->xip_ok = ok;
    if (ok != was)
        chip_rebuild_pages(m);
}

static uint32_t ssi_read(chip_t *m, uint32_t off, bool peek)
{
    ssi_t *s = &m->rp.ssi;
    if (!peek)
        ssi_settle(m);
    switch (off) {
    case 0x00: return s->ctrlr0;
    case 0x04: return s->ctrlr1;
    case 0x08: return s->ssienr;
    case 0x10: return s->ser;
    case 0x14: return s->baudr;
    case 0x18: return s->txftlr;
    case 0x1c: return s->rxftlr;
    case 0x20: return 0;
    case 0x24: return (uint32_t)s->rxn;
    case 0x28: {
        bool busy = s->auto_cs && m->now < s->busy_until;
        return (busy ? 1u : 0) | 2u | 4u | (s->rxn ? 8u : 0) | (s->rxn == 16 ? 16u : 0);
    }
    case 0x2c: return s->imr;
    case 0x30:
    case 0x34: return (s->rxn > (int)s->rxftlr ? 0x10u : 0) | 1u | (s->rx_over ? 8u : 0);
    case 0x3c:
    case 0x48:
        if (!peek)
            s->rx_over = false;
        return 0;
    case 0x4c: return s->dmacr;
    case 0x50: return s->dmatdlr;
    case 0x54: return s->dmardlr;
    case 0x58: return 0x51535049u;
    case 0x5c: return 0x3430312au;
    case 0xf0: return s->rx_sample_dly;
    case 0xf4: return s->spi_ctrlr0;
    case 0xf8: return s->txd_drive_edge;
    default:
        if (off >= 0x60 && off < 0xf0) {
            if (!s->rxn || peek)
                return s->rxn ? s->rxf[s->rxh] : 0;
            uint32_t v = s->rxf[s->rxh];
            s->rxh = (s->rxh + 1) & 15;
            s->rxn--;
            return v;
        }
        return 0;
    }
}

static void ssi_write(chip_t *m, uint32_t off, uint32_t v)
{
    ssi_t *s = &m->rp.ssi;
    switch (off) {
    case 0x00: s->ctrlr0 = v & 0x01ffffffu; break;
    case 0x04: s->ctrlr1 = v & 0xffff; break;
    case 0x08:
        s->ssienr = v & 1;
        if (!(v & 1)) {
            s->rxn = s->rxh = 0;
            s->eeprom_n = 0;
            s->auto_cs = false;
            update_select(m);
        }
        break;
    case 0x10: s->ser = v & 1; break;
    case 0x14: s->baudr = v & 0xffff; break;
    case 0x18: s->txftlr = v & 0xff; break;
    case 0x1c: s->rxftlr = v & 0xff; break;
    case 0x2c: s->imr = v & 0x3f; break;
    case 0x4c: s->dmacr = v & 3; break;
    case 0x50: s->dmatdlr = v & 0xff; break;
    case 0x54: s->dmardlr = v & 0xff; break;
    case 0xf0: s->rx_sample_dly = v & 0xff; break;
    case 0xf4: s->spi_ctrlr0 = v; break;
    case 0xf8: s->txd_drive_edge = v & 0xff; break;
    default:
        if (off >= 0x60 && off < 0xf0)
            ssi_dr_write(m, v);
        return;
    }
    ssi_eval_xip(m);
}

static uint32_t xipctrl_rp_read(chip_t *m, uint32_t off)
{
    rp2040_t *r = &m->rp;
    switch (off) {
    case 0x00: return r->xip_ctrl;
    case 0x04: return 0;
    case 0x08: return 0x3;
    case 0x0c: return m->xip_ctr_hit;
    case 0x10: return m->xip_ctr_acc;
    case 0x14: return r->xip_stream_addr;
    case 0x18: return r->xip_stream_ctr;
    default: return 0;
    }
}

static void xip_stream_dreq(chip_t *m)
{
    chip_dreq(m, DREQ_XIP_STREAM, m->rp.xip_stream_ctr != 0);
}

static void xipctrl_rp_write(chip_t *m, uint32_t off, uint32_t v)
{
    rp2040_t *r = &m->rp;
    switch (off) {
    case 0x00:
        r->xip_ctrl = v & 0xb;
        m->xip_ctrl = r->xip_ctrl;
        break;
    case 0x04: xip_flush(m); break;
    case 0x0c: m->xip_ctr_hit = 0; break;
    case 0x10: m->xip_ctr_acc = 0; break;
    case 0x14: r->xip_stream_addr = v & ~3u; break;
    case 0x18:
        r->xip_stream_ctr = v & 0x3fffff;
        xip_stream_dreq(m);
        break;
    default: break;
    }
}

// The streaming FIFO DMA reads flash through, a word at a time from STREAM_ADDR.
static uint32_t xip_stream_pop(chip_t *m)
{
    rp2040_t *r = &m->rp;
    if (!r->xip_stream_ctr)
        return 0;
    uint32_t v = r->xip_stream_addr;
    uint8_t *p = rp2040_xip_translate(m, v);
    uint32_t w = p ? rd32le(p) : 0;
    r->xip_stream_addr += 4;
    r->xip_stream_ctr--;
    xip_stream_dreq(m);
    return w;
}

uint8_t *rp2040_xip_translate(chip_t *m, uint32_t addr)
{
    unsigned alias = addr >> 24;
    if (alias < 0x10 || alias > 0x13 || !m->rp.ssi.xip_ok)
        return NULL;
    return m->flash + (addr & 0xffffff & (m->flash_size - 1));
}

static uint32_t sub_word(uint32_t word, uint32_t addr, int size)
{
    if (size == 4)
        return word;
    unsigned sh = (addr & 3) * 8;
    return size == 2 ? (word >> sh) & 0xffff : (word >> sh) & 0xff;
}

static uint32_t aliased_read(chip_t *m, uint32_t addr, bool peek)
{
    uint32_t off = addr & 0xffc;
    return (addr >> 24) == 0x14 ? xipctrl_rp_read(m, off) : ssi_read(m, off, peek);
}

static void aliased_write(chip_t *m, uint32_t addr, uint32_t v)
{
    unsigned alias = (addr >> 12) & 3;
    if (alias) {
        uint32_t old = aliased_read(m, addr, true);
        v = alias == 1 ? old ^ v : alias == 2 ? old | v : old & ~v;
    }
    uint32_t off = addr & 0xffc;
    if ((addr >> 24) == 0x14)
        xipctrl_rp_write(m, off, v);
    else
        ssi_write(m, off, v);
}

uint32_t rp2040_xip_read(chip_t *m, cpu_t *c, uint32_t addr, int size, bool *ok)
{
    unsigned alias = addr >> 24;
    if (alias >= 0x10 && alias <= 0x13) {
        if (!m->rp.ssi.xip_ok) {
            if (m->trace)
                emu_log("XIP read at %08x with the flash interface not set up for it", addr);
            // A setting that works for one read (entering continuous mode) gets it here.
            ssi_eval_xip(m);
            return 0;
        }
        uint8_t *p = rp2040_xip_translate(m, addr);
        if (c)
            c->cyc += xip_cost(m, addr);
        return size == 4 ? rd32le(p) : size == 2 ? rd16le(p) : *p;
    }
    if (alias == 0x15) {
        uint32_t o = addr & 0x3fff;
        return size == 4 ? rd32le(m->xip_sram + o) : size == 2 ? rd16le(m->xip_sram + o) : m->xip_sram[o];
    }
    if (alias == 0x14 || alias == 0x18) {
        if (c)
            c->cyc += 2;
        return sub_word(aliased_read(m, addr & ~0x3003u, false), addr, size);
    }
    *ok = false;
    return 0;
}

void rp2040_xip_write(chip_t *m, uint32_t addr, uint32_t v, int size, bool *ok)
{
    unsigned alias = addr >> 24;
    if (alias >= 0x10 && alias <= 0x13)
        return;
    if (alias == 0x15) {
        uint32_t o = addr & 0x3fff;
        if (size == 4)
            wr32le(m->xip_sram + o, v);
        else if (size == 2)
            wr16le(m->xip_sram + o, (uint16_t)v);
        else
            m->xip_sram[o] = (uint8_t)v;
        return;
    }
    if (alias == 0x14 || alias == 0x18) {
        aliased_write(m, addr & ~0x3u, v);
        return;
    }
    *ok = false;
}

static uint32_t ioqspi_rp_read(chip_t *m, uint32_t off)
{
    if (off >= 0x30)
        return 0;
    unsigned i = off >> 3;
    if (off & 4)
        return m->rp.qspi_ctrl[i];
    if (i == 1)
        return flash_selected(m) ? 0 : (1u << 17) | (1u << 9);
    return 0;
}

static void ioqspi_rp_write(chip_t *m, uint32_t off, uint32_t v)
{
    if (off >= 0x30 || !(off & 4))
        return;
    m->rp.qspi_ctrl[off >> 3] = v & 0x3003331fu;
    update_select(m);
    ssi_eval_xip(m);
}

// GPIO_HI_IN: the QSPI pins, SCLK in bit 0 and SS in bit 1. The BOOTSEL button pulls SS low
// whenever nothing drives it.
uint32_t rp2040_qspi_in(chip_t *m)
{
    bool ss = (m->rp.qspi_ctrl[1] & 0x1f) == 0 ? !flash_selected(m) : m->boot_type != 2;
    return 0x3cu | (ss ? 2u : 0);
}

void rp2040_sio_hi_changed(chip_t *m)
{
    update_select(m);
}

// ---------------------------------------------------------------------------------------------
// GPIO bank 0

// FUNCSEL values in the RP2040's table, as the RP2350's: CLOCK (8) has no model, USB is 10.
static uint32_t funcsel_in(uint32_t f)
{
    if (f <= 7 || f == FUNC_NULL)
        return f;
    if (f == 9)
        return FUNC_USB;
    return 0x1e;
}

static uint32_t iobank_off(uint32_t off)
{
    if (off < 0xf0)
        return off;
    if (off < 0x100)
        return 0x230 + (off - 0xf0);
    if (off < 0x160) {
        unsigned grp = (off - 0x100) >> 4, r = off & 0xf;
        static const uint32_t base[6] = {0x248, 0x260, 0x278, 0x290, 0x2a8, 0x2c0};
        return base[grp] + r;
    }
    return 0xfff;
}

static uint32_t iobank_rp_read(chip_t *m, uint32_t off)
{
    if (off < 0xf0 && (off & 4))
        return (off >> 3) < 30 ? m->rp.gpio_ctrl[off >> 3] : 0;
    uint32_t o = iobank_off(off);
    return o == 0xfff ? 0 : iobank_read(m, o);
}

static void iobank_rp_write(chip_t *m, uint32_t off, uint32_t v)
{
    if (off < 0xf0 && (off & 4)) {
        unsigned pin = off >> 3;
        if (pin >= 30)
            return;
        v &= 0x3003331fu;
        m->rp.gpio_ctrl[pin] = v;
        uint32_t internal = funcsel_in(v & 0x1f) | (((v >> 8) & 3) << 12) | (((v >> 12) & 3) << 14) |
                            (v & 0x30030000u);
        iobank_write(m, off, internal);
        return;
    }
    uint32_t o = iobank_off(off);
    if (o != 0xfff)
        iobank_write(m, o, v);
}

// ---------------------------------------------------------------------------------------------
// The APB and AHB-lite peripherals

static uint32_t pwm_off(uint32_t off)
{
    if (off < 0xa0)
        return off;
    return off <= 0xb0 ? off + 0x50 : 0xfff;
}

static uint32_t psm_bits(uint32_t v)
{
    return v & 0x1ffff;
}

uint32_t rp2040_periph_read(chip_t *m, uint32_t addr, bool peek, bool *ok)
{
    rp2040_t *r = &m->rp;
    uint32_t off = addr & 0xfff;
    switch (addr & 0xffffc000u) {
    case 0x40000000:
        switch (off) {
        case 0x00: return 0x20002927u;
        case 0x04: return 2;
        default: return 0;
        }
    case 0x40004000: return off < 0x20 ? r->syscfg[off >> 2] : 0;
    case 0x40008000: return clocks_read(m, off);
    case 0x4000c000:
        switch (off) {
        case 0x0: return r->resets;
        case 0x8: return ~r->resets & RST_ALL;
        default: return 0;
        }
    case 0x40010000:
        switch (off) {
        case 0x0: return r->psm_frce_on;
        case 0x4: return r->psm_frce_off;
        case 0x8: return r->psm_wdsel;
        case 0xc: return 0x1ffff & ~r->psm_frce_off;
        default: return 0;
        }
    case 0x40014000: return iobank_rp_read(m, off);
    case 0x40018000: return ioqspi_rp_read(m, off);
    case 0x4001c000: return pads_read(m, false, off);
    case 0x40020000: return off < 0x1c ? r->pads_qspi[off >> 2] : 0;
    case 0x40024000: return sysctl_read(m, 0x40048000, off == 0x1c ? 0x10 : off == 0x10 ? 0x1c : off);
    case 0x40028000: return sysctl_read(m, 0x40050000, off);
    case 0x4002c000: return sysctl_read(m, 0x40058000, off);
    case 0x40030000:
        if (off == 0x04)
            return 1;
        return off < 0x28 ? r->busctrl[off >> 2] : 0;
    case 0x40034000: return pl011_read(m, &m->uart[0], off, peek);
    case 0x40038000: return pl011_read(m, &m->uart[1], off, peek);
    case 0x4003c000: return pl022_read(m, &m->spi[0], off, peek);
    case 0x40040000: return pl022_read(m, &m->spi[1], off, peek);
    case 0x40044000: return dw_i2c_read(m, &m->i2c[0], off, peek);
    case 0x40048000: return dw_i2c_read(m, &m->i2c[1], off, peek);
    case 0x4004c000: return sysctl_read(m, 0x400a0000, off);
    case 0x40050000: {
        uint32_t o = pwm_off(off);
        return o == 0xfff ? 0 : pwm_read(m, o);
    }
    case 0x40054000: return timer_read(m, &m->timer[0], off >= 0x34 ? off + 8 : off, peek);
    case 0x40058000:
        if (off == 0x2c) {
            bool run = (r->wd_tick & (1u << 9)) && m->ref_hz;
            return r->wd_tick | (run ? 1u << 10 : 0);
        }
        return watchdog_read(m, off);
    case 0x4005c000: return rtc_read(m, off);
    case 0x40060000: return sysctl_read(m, 0x400e8000, off >= 0x0c ? off + 4 : off);
    case 0x40064000:
        switch (off) {
        case 0x0: return r->vreg | (1u << 12);
        case 0x4: return r->bod;
        case 0x8: return r->chip_reset;
        default: return 0;
        }
    case 0x4006c000: return off == 0 ? 1 : 0;
    default: break;
    }
    switch (addr & 0xfff00000u) {
    case 0x50000000: return dma_read(m, off, peek);
    case 0x50100000:
        if ((addr & 0xf0000) == 0x10000)
            return m->usb_regs[(off >> 2) & 63];
        break;
    case 0x50200000: return pio_read(m, &m->pio[0], off, peek);
    case 0x50300000: return pio_read(m, &m->pio[1], off, peek);
    case 0x50400000: return peek ? 0 : xip_stream_pop(m);
    default: break;
    }
    if ((addr >> 28) == 5)
        *ok = false;
    return 0;
}

void rp2040_periph_write(chip_t *m, uint32_t addr, uint32_t v, bool *ok)
{
    rp2040_t *r = &m->rp;
    uint32_t off = addr & 0xfff;
    switch (addr & 0xffffc000u) {
    case 0x40000000: return;
    case 0x40004000:
        if (off < 0x20)
            r->syscfg[off >> 2] = v;
        return;
    case 0x40008000: clocks_write(m, off, v); return;
    case 0x4000c000:
        if (off == 0)
            set_resets(m, v);
        return;
    case 0x40010000:
        switch (off) {
        case 0x0: r->psm_frce_on = psm_bits(v); break;
        case 0x4:
            r->psm_frce_off = psm_bits(v);
            chip_core1_power(m, (v >> 16) & 1);
            break;
        case 0x8: r->psm_wdsel = psm_bits(v); break;
        default: break;
        }
        return;
    case 0x40014000: iobank_rp_write(m, off, v); return;
    case 0x40018000: ioqspi_rp_write(m, off, v); return;
    case 0x4001c000: pads_write(m, false, off, v & 0xff); return;
    case 0x40020000:
        if (off < 0x1c)
            r->pads_qspi[off >> 2] = v & 0xff;
        return;
    case 0x40024000: sysctl_write(m, 0x40048000, off == 0x1c ? 0x10 : off == 0x10 ? 0x1c : off, v); return;
    case 0x40028000: sysctl_write(m, 0x40050000, off, v); return;
    case 0x4002c000: sysctl_write(m, 0x40058000, off, v); return;
    case 0x40030000:
        // BUS_PRIORITY, and four performance counters (cleared by any write) with their selectors.
        if (off == 0x00)
            r->busctrl[0] = v & 0x1111;
        else if (off >= 0x08 && off < 0x28)
            r->busctrl[off >> 2] = (off & 4) ? v & 0x1f : 0;
        return;
    case 0x40034000: pl011_write(m, &m->uart[0], off, v); return;
    case 0x40038000: pl011_write(m, &m->uart[1], off, v); return;
    case 0x4003c000: pl022_write(m, &m->spi[0], off, v); return;
    case 0x40040000: pl022_write(m, &m->spi[1], off, v); return;
    case 0x40044000: dw_i2c_write(m, &m->i2c[0], off, v); return;
    case 0x40048000: dw_i2c_write(m, &m->i2c[1], off, v); return;
    case 0x4004c000: sysctl_write(m, 0x400a0000, off, v); return;
    case 0x40050000: {
        uint32_t o = pwm_off(off);
        if (o != 0xfff)
            pwm_write(m, o, v);
        return;
    }
    case 0x40054000: timer_write(m, &m->timer[0], off >= 0x34 ? off + 8 : off, v); return;
    case 0x40058000:
        if (off == 0x2c)
            wd_tick_write(m, v);
        else
            watchdog_write(m, off, v);
        return;
    case 0x4005c000: rtc_write(m, off, v); return;
    case 0x40060000: sysctl_write(m, 0x400e8000, off >= 0x0c ? off + 4 : off, v); return;
    case 0x40064000:
        switch (off) {
        case 0x0: r->vreg = v & 0xf3; break;
        case 0x4: r->bod = v & 0xf1; break;
        case 0x8:
            if (v & (1u << 24))
                r->chip_reset &= ~(1u << 24);
            break;
        default: break;
        }
        return;
    case 0x4006c000: return;
    default: break;
    }
    switch (addr & 0xfff00000u) {
    case 0x50000000: dma_write(m, off, v); return;
    case 0x50100000:
        if ((addr & 0xf0000) == 0x10000) {
            m->usb_regs[(off >> 2) & 63] = v;
            return;
        }
        break;
    case 0x50200000: pio_write(m, &m->pio[0], off, v); return;
    case 0x50300000: pio_write(m, &m->pio[1], off, v); return;
    case 0x50400000: return;
    default: break;
    }
    if ((addr >> 28) == 5)
        *ok = false;
}
