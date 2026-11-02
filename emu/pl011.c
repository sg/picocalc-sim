#include <string.h>

#include "chip.h"

// ARM PrimeCell PL011 UART. Transmitted bytes go to the host at once, but the FIFO and BUSY flags
// follow the configured baud rate so firmware waiting on them waits as long as on the chip.

extern bool g_fast_bus;

void pl011_reset(pl011_t *s, int idx)
{
    memset(s, 0, sizeof(*s));
    s->idx = idx;
    s->cr = 0x300;
    s->ifls = 0x12;
}

static uint64_t char_ps(chip_t *m, pl011_t *s)
{
    if (g_fast_bus)
        return 0;
    uint64_t div64 = (uint64_t)s->ibrd * 64 + s->fbrd;
    if (!div64 || !m->peri_hz)
        return 0;
    unsigned bits = 1 + 5 + ((s->lcr_h >> 5) & 3) + ((s->lcr_h & 8) ? 2 : 1) + ((s->lcr_h & 2) ? 1 : 0);
    // baud = clk / (16 * div), and div64 = 64 * div.
    return (uint64_t)bits * 16 * div64 * PS_PER_S / (64 * m->peri_hz);
}

static int tx_level(chip_t *m, pl011_t *s)
{
    if (s->tx_done <= m->now)
        return 0;
    uint64_t cp = char_ps(m, s);
    if (!cp)
        return 0;
    return (int)((s->tx_done - m->now + cp - 1) / cp);
}

static int fifo_depth(pl011_t *s)
{
    return (s->lcr_h & 0x10) ? 32 : 1;
}

static void update_irq(chip_t *m, pl011_t *s)
{
    static const int levels[5] = {4, 8, 16, 24, 28};
    int tl = levels[(s->ifls & 7) > 4 ? 4 : (s->ifls & 7)];
    int rl = levels[((s->ifls >> 3) & 7) > 4 ? 4 : ((s->ifls >> 3) & 7)];
    if (!(s->lcr_h & 0x10))
        tl = rl = 1;
    s->ris &= ~0x70u;
    if (tx_level(m, s) <= tl)
        s->ris |= 0x20;
    if (s->rxn >= rl)
        s->ris |= 0x10;
    if (s->rxn > 0)
        s->ris |= 0x40;
    chip_irq(m, IRQ_UART0 + (unsigned)s->idx, (s->ris & s->imsc) != 0);
    chip_dreq(m, DREQ_UART0_TX + 2u * (unsigned)s->idx, (s->dmacr & 2) && tx_level(m, s) < fifo_depth(s));
    chip_dreq(m, DREQ_UART0_RX + 2u * (unsigned)s->idx, (s->dmacr & 1) && s->rxn > 0);
}

static void uart_event(chip_t *m, emu_event_t *ev)
{
    update_irq(m, ev->ctx);
}

void pl011_rx(chip_t *m, int idx, uint8_t byte)
{
    pl011_t *s = &m->uart[idx];
    if (!(s->cr & 1) || !(s->cr & 0x200))
        return;
    if (s->rxn < fifo_depth(s)) {
        s->rxf[(s->rxh + s->rxn) & 31] = byte;
        s->rxn++;
    } else {
        s->ris |= 1u << 10;
    }
    update_irq(m, s);
}

uint32_t pl011_read(chip_t *m, pl011_t *s, uint32_t off, bool peek)
{
    switch (off) {
    case 0x00: {
        if (!s->rxn)
            return 0;
        uint32_t v = s->rxf[s->rxh];
        if (!peek) {
            s->rxh = (s->rxh + 1) & 31;
            s->rxn--;
            update_irq(m, s);
        }
        return v;
    }
    case 0x18: {
        int tl = tx_level(m, s);
        return (tl > 0 ? 8u : 0) | (s->rxn == 0 ? 0x10u : 0) | (tl >= fifo_depth(s) ? 0x20u : 0) |
               (s->rxn >= fifo_depth(s) ? 0x40u : 0) | (tl == 0 ? 0x80u : 0) | 1u;
    }
    case 0x24: return s->ibrd;
    case 0x28: return s->fbrd;
    case 0x2c: return s->lcr_h;
    case 0x30: return s->cr;
    case 0x34: return s->ifls;
    case 0x38: return s->imsc;
    case 0x3c:
        update_irq(m, s);
        return s->ris;
    case 0x40:
        update_irq(m, s);
        return s->ris & s->imsc;
    case 0x48: return s->dmacr;
    case 0xfe0: return 0x11;
    case 0xfe4: return 0x10;
    case 0xfe8: return 0x34;
    case 0xfec: return 0x00;
    case 0xff0: return 0x0d;
    case 0xff4: return 0xf0;
    case 0xff8: return 0x05;
    case 0xffc: return 0xb1;
    default: return 0;
    }
}

void pl011_write(chip_t *m, pl011_t *s, uint32_t off, uint32_t v)
{
    switch (off) {
    case 0x00:
        if ((s->cr & 1) && (s->cr & 0x100) && tx_level(m, s) < fifo_depth(s)) {
            uint64_t start = s->tx_done > m->now ? s->tx_done : m->now;
            s->tx_done = start + char_ps(m, s);
            devices_uart_tx(m, s->idx, (uint8_t)v);
            if (s->imsc & 0x20) {
                s->ev.fn = uart_event;
                s->ev.ctx = s;
                ev_schedule(m, &s->ev, s->tx_done);
            }
        }
        break;
    case 0x24: s->ibrd = v & 0xffff; break;
    case 0x28: s->fbrd = v & 0x3f; break;
    case 0x2c: s->lcr_h = v & 0xff; break;
    case 0x30: s->cr = v & 0xff87; break;
    case 0x34: s->ifls = v & 0x3f; break;
    case 0x38: s->imsc = v & 0x7ff; break;
    case 0x44: s->ris &= ~v; break;
    case 0x48: s->dmacr = v & 7; break;
    default: break;
    }
    update_irq(m, s);
}
