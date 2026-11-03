#include <string.h>

#include "chip.h"

// ARM PrimeCell PL022 SSP in Motorola SPI master mode. Frames leave the TX FIFO at the bit rate
// the driver configured; the exchange with whichever device is selected happens when each frame
// finishes shifting, so the CPU sees FIFO levels and BSY exactly as it would on the chip.

bool g_fast_bus;

void pl022_reset(pl022_t *s, int idx)
{
    memset(s, 0, sizeof(*s));
    s->idx = idx;
}

static uint64_t frame_ps(chip_t *m, pl022_t *s)
{
    if (g_fast_bus)
        return 0;
    uint32_t cpsdvsr = s->cpsr & 0xfe;
    uint32_t scr = (s->cr0 >> 8) & 0xff;
    unsigned bits = (s->cr0 & 0xf) + 1;
    if (!cpsdvsr || !m->peri_hz)
        return UINT64_MAX / 4;
    return (uint64_t)bits * cpsdvsr * (1 + scr) * PS_PER_S / m->peri_hz;
}

static bool enabled(pl022_t *s)
{
    return s->cr1 & 2;
}

static void update_irq(chip_t *m, pl022_t *s)
{
    s->ris &= 3;
    if (s->txn <= 4)
        s->ris |= 8;
    if (s->rxn >= 4)
        s->ris |= 4;
    chip_irq(m, IRQ_SPI0 + (unsigned)s->idx, (s->ris & s->imsc) != 0);
    chip_dreq(m, DREQ_SPI0_TX + 2u * (unsigned)s->idx, (s->dmacr & 2) && s->txn < 8);
    chip_dreq(m, DREQ_SPI0_RX + 2u * (unsigned)s->idx, (s->dmacr & 1) && s->rxn > 0);
}

static void finish_frame(chip_t *m, pl022_t *s)
{
    uint16_t mosi = s->txf[s->txh];
    s->txh = (s->txh + 1) & 7;
    s->txn--;
    unsigned bits = (s->cr0 & 0xf) + 1;
    uint16_t miso = (uint16_t)((1u << bits) - 1);
    if (s->cr1 & 1)
        miso = mosi;
    else
        devices_spi_frame(m, s->idx, mosi, (int)bits, &miso);
    if (s->rxn < 8) {
        s->rxf[(s->rxh + s->rxn) & 7] = miso & (uint16_t)((1u << bits) - 1);
        s->rxn++;
    } else {
        s->ris |= 1;
    }
}

static void spi_event(chip_t *m, emu_event_t *ev);

static void sync(chip_t *m, pl022_t *s)
{
    if (!enabled(s))
        return;
    uint64_t fp = frame_ps(m, s);
    bool any = false;
    while (s->txn > 0 && s->busy_until + fp <= m->now) {
        s->busy_until += fp;
        finish_frame(m, s);
        any = true;
    }
    if (s->txn == 0 && s->busy_until < m->now)
        s->busy_until = m->now;
    if (any)
        update_irq(m, s);
    if (s->txn > 0 && (s->imsc || s->dmacr)) {
        s->ev.fn = spi_event;
        s->ev.ctx = s;
        ev_schedule(m, &s->ev, s->busy_until + fp);
    } else {
        ev_cancel(m, &s->ev);
    }
}

static void spi_event(chip_t *m, emu_event_t *ev)
{
    sync(m, ev->ctx);
}

void pl022_sync_all(chip_t *m)
{
    sync(m, &m->spi[0]);
    sync(m, &m->spi[1]);
}

bool pl022_cs_low(chip_t *m, int idx)
{
    pl022_t *s = &m->spi[idx];
    return enabled(s) && s->txn > 0 && s->busy_until <= m->now;
}

uint32_t pl022_read(chip_t *m, pl022_t *s, uint32_t off, bool peek)
{
    if (!peek)
        sync(m, s);
    switch (off) {
    case 0x00: return s->cr0;
    case 0x04: return s->cr1;
    case 0x08: {
        if (!s->rxn || peek)
            return s->rxn ? s->rxf[s->rxh] : 0;
        uint16_t v = s->rxf[s->rxh];
        s->rxh = (s->rxh + 1) & 7;
        s->rxn--;
        update_irq(m, s);
        return v;
    }
    case 0x0c: {
        bool busy = enabled(s) && s->txn > 0;
        return (s->txn == 0 ? 1u : 0) | (s->txn < 8 ? 2u : 0) | (s->rxn > 0 ? 4u : 0) | (s->rxn == 8 ? 8u : 0) |
               (busy ? 16u : 0);
    }
    case 0x10: return s->cpsr;
    case 0x14: return s->imsc;
    case 0x18: return s->ris;
    case 0x1c: return s->ris & s->imsc;
    case 0x24: return s->dmacr;
    case 0xfe0: return 0x22;
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

void pl022_write(chip_t *m, pl022_t *s, uint32_t off, uint32_t v)
{
    sync(m, s);
    switch (off) {
    case 0x00: s->cr0 = v & 0xffff; break;
    case 0x04: {
        bool was = enabled(s);
        s->cr1 = v & 0xf;
        if (!was && enabled(s) && s->busy_until < m->now)
            s->busy_until = m->now;
        break;
    }
    case 0x08:
        if (s->txn < 8) {
            if (s->txn == 0 && s->busy_until < m->now)
                s->busy_until = m->now;
            s->txf[(s->txh + s->txn) & 7] = (uint16_t)v;
            s->txn++;
        }
        break;
    case 0x10: s->cpsr = v & 0xff; break;
    case 0x14: s->imsc = v & 0xf; break;
    case 0x20: s->ris &= ~(v & 3); break;
    case 0x24: s->dmacr = v & 3; break;
    default: break;
    }
    update_irq(m, s);
    sync(m, s);
}
