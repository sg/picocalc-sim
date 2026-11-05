#include <string.h>

#include "chip.h"

// ACCESSCTRL (datasheet section 10.6): which bus managers may reach each bus endpoint and at which
// security and privilege levels, and which GPIOs Non-secure code may use.

enum {
    AC_LOCK,
    AC_FORCE_CORE_NS,
    AC_CFGRESET,
    AC_NSMASK0,
    AC_NSMASK1,
    AC_ROM,
    AC_XIP_MAIN,
    AC_SRAM0,
    AC_SRAM4 = AC_SRAM0 + 4,
    AC_SRAM8 = AC_SRAM0 + 8,
    AC_SRAM9,
    AC_DMA,
    AC_USBCTRL,
    AC_PIO0,
    AC_PIO1,
    AC_PIO2,
    AC_CORESIGHT_TRACE,
    AC_CORESIGHT_PERIPH,
    AC_SYSINFO,
    AC_RESETS,
    AC_IO_BANK0,
    AC_IO_BANK1,
    AC_PADS_BANK0,
    AC_PADS_QSPI,
    AC_BUSCTRL,
    AC_ADC,
    AC_HSTX,
    AC_I2C0,
    AC_I2C1,
    AC_PWM,
    AC_SPI0,
    AC_SPI1,
    AC_TIMER0,
    AC_TIMER1,
    AC_UART0,
    AC_UART1,
    AC_OTP,
    AC_TBMAN,
    AC_POWMAN,
    AC_TRNG,
    AC_SHA256,
    AC_SYSCFG,
    AC_CLOCKS,
    AC_XOSC,
    AC_ROSC,
    AC_PLL_SYS,
    AC_PLL_USB,
    AC_TICKS,
    AC_WATCHDOG,
    AC_PSM,
    AC_XIP_CTRL,
    AC_XIP_QMI,
    AC_XIP_AUX,
    AC_COUNT,
};

// No filter, and boot RAM's hardwired Secure-only access.
#define EP_NONE 0
#define EP_SECURE 0xff

// The bits of a bus endpoint's register: managers, then security and privilege levels.
#define P_DMA 0x40u
#define P_CORE1 0x20u
#define P_CORE0 0x10u
#define P_SP 0x08u
#define P_SU 0x04u
#define P_NSP 0x02u
#define P_NSU 0x01u

// The datasheet's reset values: ROM, XIP, SRAM and SYSINFO are open to everything, the clocks,
// power and XIP control are Secure privileged only and out of the DMA's reach, and the rest is
// Secure only.
static const uint8_t reset_value[AC_COUNT] = {
    [AC_LOCK] = 4,
    [AC_ROM ... AC_SRAM9] = 0xff,
    [AC_DMA ... AC_PIO2] = 0xfc,
    [AC_CORESIGHT_TRACE] = 0xb8,
    [AC_CORESIGHT_PERIPH] = 0xb8,
    [AC_SYSINFO] = 0xff,
    [AC_RESETS ... AC_TBMAN] = 0xfc,
    [AC_POWMAN] = 0xb8,
    [AC_TRNG] = 0xb8,
    [AC_SHA256] = 0xf8,
    [AC_SYSCFG ... AC_XIP_QMI] = 0xb8,
    [AC_XIP_AUX] = 0xf8,
};

// APB blocks are 32 KB apart from 0x40000000, the first 2 MB of them.
static const uint8_t apb_endpoint[64] = {
    AC_SYSINFO, AC_SYSCFG, AC_CLOCKS,   AC_PSM,      AC_RESETS,  AC_IO_BANK0, AC_IO_BANK1, AC_PADS_BANK0,
    AC_PADS_QSPI, AC_XOSC, AC_PLL_SYS,  AC_PLL_USB,  EP_NONE,    AC_BUSCTRL,  AC_UART0,    AC_UART1,
    AC_SPI0,    AC_SPI1,   AC_I2C0,     AC_I2C1,     AC_ADC,     AC_PWM,      AC_TIMER0,   AC_TIMER1,
    AC_HSTX,    AC_XIP_CTRL, AC_XIP_QMI, AC_WATCHDOG, EP_SECURE, AC_ROSC,     AC_TRNG,     AC_SHA256,
    AC_POWMAN,  AC_TICKS,  EP_NONE,     EP_NONE,     AC_OTP,     AC_OTP,      AC_OTP,      AC_OTP,
    AC_CORESIGHT_PERIPH, AC_CORESIGHT_PERIPH, EP_NONE, EP_NONE, AC_TBMAN,
};

static const uint8_t ahb_endpoint[8] = {
    AC_DMA, AC_USBCTRL, AC_PIO0, AC_PIO1, AC_PIO2, AC_XIP_AUX, AC_HSTX, AC_CORESIGHT_TRACE,
};

static unsigned endpoint(uint32_t a)
{
    switch (a >> 28) {
    case 0x0: return AC_ROM;
    case 0x1: return AC_XIP_MAIN;
    case 0x2:
        // SRAM0-3 and SRAM4-7 are each striped word by word.
        if (a < 0x20080000u)
            return (a < 0x20040000u ? AC_SRAM0 : AC_SRAM4) + ((a >> 2) & 3);
        if (a < 0x20082000u)
            return a < 0x20081000u ? AC_SRAM8 : AC_SRAM9;
        return EP_NONE;
    case 0x4: return a < 0x40200000u ? apb_endpoint[(a >> 15) & 63] : EP_NONE;
    case 0x5: return a < 0x50800000u ? ahb_endpoint[(a >> 20) & 7] : EP_NONE;
    default: return EP_NONE;
    }
}

static inline uint32_t level_bits(bool ns, bool priv)
{
    return ns ? (priv ? P_NSP : P_NSP | P_NSU) : (priv ? P_SP : P_SP | P_SU);
}

static bool allows(chip_t *m, unsigned ep, uint32_t manager, bool ns, bool priv)
{
    if (ep == EP_NONE)
        return true;
    if (ep == EP_SECURE)
        return !ns;
    uint32_t need = manager | level_bits(ns, priv);
    return (m->accessctrl[ep] & need) == need;
}

static void update_derived(chip_t *m)
{
    uint32_t any_secure = P_CORE0 | P_CORE1 | P_SP | P_SU, cores_sp = P_CORE0 | P_CORE1 | P_SP;
    m->ac_timer_open = (m->accessctrl[AC_TIMER0] & m->accessctrl[AC_TIMER1] & any_secure) == any_secure;
    m->ac_sp_open = true;
    for (unsigned i = AC_ROM; i < AC_COUNT; i++) {
        uint32_t need = i < AC_DMA ? cores_sp | P_NSP : cores_sp;
        if ((m->accessctrl[i] & need) != need)
            m->ac_sp_open = false;
    }
    cpu_t *c1 = &m->core[1];
    c1->bus_ns = c1->ns || (m->accessctrl[AC_FORCE_CORE_NS] & 2);
}

void accessctrl_reset(chip_t *m, bool full)
{
    for (unsigned i = AC_NSMASK0; i < AC_COUNT; i++)
        m->accessctrl[i] = reset_value[i];
    if (full) {
        m->accessctrl[AC_LOCK] = reset_value[AC_LOCK];
        m->accessctrl[AC_FORCE_CORE_NS] = 0;
    }
    update_derived(m);
}

bool accessctrl_core_allows(cpu_t *c, uint32_t a)
{
    chip_t *m = c->m;
    unsigned ep = endpoint(a);
    if (ep == EP_NONE)
        return true;
    bool ns = c->bus_ns;
    // Secure loads and stores to memory the SAU calls Non-secure are Non-secure transactions.
    if (!ns && ep < AC_DMA && (c->sau_ctrl & 3))
        ns = cpu_attr_ns(c, a);
    return allows(m, ep, c->id ? P_CORE1 : P_CORE0, ns, cpu_privileged(c));
}

bool accessctrl_dma_allows(chip_t *m, uint32_t a, uint32_t seccfg)
{
    return allows(m, endpoint(a), P_DMA, !(seccfg & 2), seccfg & 1);
}

bool accessctrl_ns_reaches(chip_t *m, unsigned ep, bool priv)
{
    uint32_t need = level_bits(true, priv);
    return ep != EP_NONE && ep != EP_SECURE && (m->accessctrl[ep] & need) == need;
}

// The endpoint a GPIO function on a pin belongs to, by the datasheet's function table.
static unsigned func_endpoint(unsigned pin, unsigned f)
{
    switch (f) {
    case FUNC_HSTX: return AC_HSTX;
    case FUNC_SPI: return (pin >> 3) & 1 ? AC_SPI1 : AC_SPI0;
    case FUNC_UART:
    case FUNC_UART_AUX: return ((pin >> 2) ^ (pin >> 3)) & 1 ? AC_UART1 : AC_UART0;
    case FUNC_I2C: return (pin >> 1) & 1 ? AC_I2C1 : AC_I2C0;
    case FUNC_PWM: return AC_PWM;
    case FUNC_PIO0:
    case FUNC_PIO1:
    case FUNC_PIO2: return AC_PIO0 + f - FUNC_PIO0;
    case FUNC_XIP_CS1:
        if (pin == 0 || pin == 8 || pin == 19)
            return AC_XIP_QMI;
        return pin >= 1 && pin <= 5 ? AC_CORESIGHT_TRACE : AC_CLOCKS;
    case FUNC_USB: return AC_USBCTRL;
    default: return EP_NONE;
    }
}

bool accessctrl_func_allowed(chip_t *m, unsigned pin, unsigned f)
{
    unsigned ep = func_endpoint(pin, f);
    return ep == EP_NONE || !gpio_ns(m, pin) || (m->accessctrl[ep] & P_NSP);
}

static const uint8_t reset_endpoint[29] = {
    AC_ADC,    AC_BUSCTRL, AC_DMA,    AC_HSTX,    AC_I2C0,  AC_I2C1,   AC_IO_BANK0, AC_IO_BANK1, EP_SECURE, AC_PADS_BANK0,
    AC_PADS_QSPI, AC_PIO0, AC_PIO1,   AC_PIO2,    AC_PLL_SYS, AC_PLL_USB, AC_PWM,   AC_SHA256,   AC_SPI0,   AC_SPI1,
    AC_SYSCFG, AC_SYSINFO, AC_TBMAN,  AC_TIMER0,  AC_TIMER1, AC_TRNG,  AC_UART0,    AC_UART1,    AC_USBCTRL,
};

uint32_t accessctrl_resets_ns_mask(chip_t *m, bool priv)
{
    uint32_t mask = 0;
    for (unsigned i = 0; i < 29; i++)
        if (accessctrl_ns_reaches(m, reset_endpoint[i], priv))
            mask |= 1u << i;
    return mask;
}

uint32_t accessctrl_read(chip_t *m, uint32_t off)
{
    return off < 4 * AC_COUNT ? m->accessctrl[off >> 2] : 0;
}

bool accessctrl_write(chip_t *m, cpu_t *c, uint32_t addr, uint32_t data)
{
    unsigned off = addr & 0xfff, alias = (addr >> 12) & 3, i = off >> 2;
    // The DMA and unprivileged code may not write, and every register but the GPIO masks wants
    // the password, which an atomic alias's data must carry too.
    if (!c || !cpu_privileged(c))
        return false;
    bool mask_reg = i == AC_NSMASK0 || i == AC_NSMASK1;
    if (!mask_reg && (data >> 16) != 0xacce)
        return false;
    if (i >= AC_COUNT || (m->accessctrl[AC_LOCK] & (1u << c->id)))
        return true;
    uint32_t old = m->accessctrl[i], v = mask_reg ? data : data & 0xffff;
    switch (alias) {
    case 1: v ^= old; break;
    case 2: v |= old; break;
    case 3: v = old & ~v; break;
    default: break;
    }
    // Non-secure code may only pass on to its own unprivileged code what Secure code granted it.
    if (c->bus_ns) {
        if (i < AC_ROM || !(old & P_NSP))
            return true;
        v = (old & ~P_NSU) | (v & P_NSU);
    }
    switch (i) {
    case AC_LOCK: m->accessctrl[i] = old | (v & 0xb); break;
    case AC_FORCE_CORE_NS: m->accessctrl[i] = v & 2; break;
    case AC_CFGRESET:
        if (v & 1)
            accessctrl_reset(m, false);
        break;
    case AC_NSMASK0: m->accessctrl[i] = v; break;
    case AC_NSMASK1: m->accessctrl[i] = v & 0xff00ffffu; break;
    default: m->accessctrl[i] = v & 0xff; break;
    }
    update_derived(m);
    gpio_security_changed(m);
    return true;
}
