#include <string.h>

#include "chip.h"

// The 16-channel DMA. Transfers move as soon as their DREQ allows; peripherals model their own
// FIFO timing, so a channel paced by SPI or PIO runs at the peripheral's rate. Pacing timers
// schedule their own transfers.

#define CH_EN (1u << 0)
#define CH_BUSY (1u << 26)

static bool running;
static bool rekick;
static emu_event_t timer_ev[4];
static uint64_t timer_next[4];

void dma_reset(dma_t *d)
{
    memset(d, 0, sizeof(*d));
    for (int i = 0; i < 16; i++)
        d->ch[i].ctrl = (uint32_t)i << 13;
    running = rekick = false;
}

static void update_irq(chip_t *m)
{
    dma_t *d = &m->dma;
    for (int k = 0; k < 4; k++)
        chip_irq(m, IRQ_DMA_0 + (unsigned)k, ((d->intr & d->inte[k]) | d->intf[k]) != 0);
}

static void trigger(chip_t *m, int ch)
{
    dma_ch_t *c = &m->dma.ch[ch];
    if (!(c->ctrl & CH_EN))
        return;
    c->trans_count = c->count_reload;
    if ((c->trans_count & 0x0fffffffu) == 0 && (c->trans_count >> 28) != 0xf) {
        if (!(c->ctrl & (1u << 23)))
            m->dma.intr |= 1u << ch;
        update_irq(m);
        return;
    }
    c->ctrl |= CH_BUSY;
}

static bool dreq_ready(chip_t *m, int ch)
{
    unsigned sel = (m->dma.ch[ch].ctrl >> 17) & 0x3f;
    if (sel == 0x3f)
        return true;
    if (sel >= 0x3b && sel <= 0x3e)
        return false;
    return sel < DREQ_COUNT && m->dreq[sel];
}

static uint32_t step_addr(uint32_t addr, uint32_t size, bool incr, bool rev, unsigned ring, bool ring_this)
{
    if (!incr)
        return addr;
    uint32_t next = rev ? addr - size : addr + size;
    if (ring_this && ring) {
        uint32_t mask = (1u << ring) - 1;
        next = (addr & ~mask) | (next & mask);
    }
    return next;
}

static void transfer_one(chip_t *m, int ch)
{
    dma_t *d = &m->dma;
    dma_ch_t *c = &d->ch[ch];
    uint32_t ctrl = c->ctrl;
    uint32_t size = 1u << ((ctrl >> 2) & 3);
    bool ok = true;
    uint32_t v = sys_read(m, c->read_addr, (int)size, &ok);
    if (ctrl & (1u << 24)) {
        if (size == 4)
            v = __builtin_bswap32(v);
        else if (size == 2)
            v = (uint32_t)__builtin_bswap16((uint16_t)v);
    }
    if (ctrl & (1u << 25)) {
        if (((d->sniff_ctrl >> 1) & 0xf) == 0 && ((d->sniff_ctrl >> 1) & 0x3f) == (uint32_t)ch)
            d->sniff_data += v;
    }
    if (ok)
        sys_write(m, c->write_addr, v, (int)size, &ok);
    if (!ok)
        c->ctrl |= (1u << 31) | (1u << 30);
    unsigned ring = (ctrl >> 8) & 0xf;
    bool ring_write = ctrl & (1u << 12);
    c->read_addr = step_addr(c->read_addr, size, ctrl & (1u << 4), ctrl & (1u << 5), ring, !ring_write);
    c->write_addr = step_addr(c->write_addr, size, ctrl & (1u << 6), ctrl & (1u << 7), ring, ring_write);
    if ((c->trans_count >> 28) != 0xf)
        c->trans_count = (c->trans_count & 0xf0000000u) | ((c->trans_count - 1) & 0x0fffffffu);
    if ((c->trans_count & 0x0fffffffu) == 0 && (c->trans_count >> 28) != 0xf) {
        c->ctrl &= ~CH_BUSY;
        if (!(ctrl & (1u << 23)))
            d->intr |= 1u << ch;
        update_irq(m);
        unsigned chain = (ctrl >> 13) & 0xf;
        if ((c->trans_count >> 28) == 1)
            trigger(m, ch);
        else if ((int)chain != ch)
            trigger(m, (int)chain);
    }
}

void dma_kick(chip_t *m)
{
    if (running) {
        rekick = true;
        return;
    }
    running = true;
    dma_t *d = &m->dma;
    int guard = 0;
    do {
        rekick = false;
        bool progress = true;
        while (progress && guard < 1 << 22) {
            progress = false;
            for (int ch = 0; ch < 16; ch++) {
                if (!(d->ch[ch].ctrl & CH_BUSY) || !dreq_ready(m, ch))
                    continue;
                transfer_one(m, ch);
                progress = true;
                guard++;
            }
        }
    } while (rekick && guard < 1 << 22);
    running = false;
}

static uint64_t timer_period(chip_t *m, int t)
{
    uint32_t x = m->dma.timer[t] >> 16, y = m->dma.timer[t] & 0xffff;
    if (!x || !y)
        return 0;
    return (uint64_t)m->sys_ps * y / x;
}

static void timer_fire(chip_t *m, emu_event_t *ev)
{
    int t = (int)(ev - timer_ev);
    dma_t *d = &m->dma;
    for (int ch = 0; ch < 16; ch++) {
        dma_ch_t *c = &d->ch[ch];
        if ((c->ctrl & CH_BUSY) && ((c->ctrl >> 17) & 0x3f) == (uint32_t)(0x3b + t)) {
            running = true;
            transfer_one(m, ch);
            running = false;
        }
    }
    dma_kick(m);
    uint64_t p = timer_period(m, t);
    bool any = false;
    for (int ch = 0; ch < 16; ch++)
        if ((d->ch[ch].ctrl & CH_BUSY) && ((d->ch[ch].ctrl >> 17) & 0x3f) == (uint32_t)(0x3b + t))
            any = true;
    if (p && any) {
        timer_next[t] += p;
        if (timer_next[t] < m->now)
            timer_next[t] = m->now;
        timer_ev[t].fn = timer_fire;
        ev_schedule(m, &timer_ev[t], timer_next[t]);
    }
}

static void timers_arm(chip_t *m)
{
    for (int t = 0; t < 4; t++) {
        bool any = false;
        for (int ch = 0; ch < 16; ch++)
            if ((m->dma.ch[ch].ctrl & CH_BUSY) && ((m->dma.ch[ch].ctrl >> 17) & 0x3f) == (uint32_t)(0x3b + t))
                any = true;
        uint64_t p = timer_period(m, t);
        if (any && p && !timer_ev[t].queued) {
            timer_next[t] = m->now + p;
            timer_ev[t].fn = timer_fire;
            ev_schedule(m, &timer_ev[t], timer_next[t]);
        }
    }
}

// A channel paced by a PIO state machine moves only as the state machine catches up, which a CPU
// waiting on the channel would otherwise not cause, so the PIO catches up before it looks.
static void sync_pacers(chip_t *m)
{
    unsigned pios = 0;
    for (int ch = 0; ch < 16; ch++) {
        uint32_t ctrl = m->dma.ch[ch].ctrl;
        unsigned sel = (ctrl >> 17) & 0x3f;
        if ((ctrl & CH_BUSY) && sel < DREQ_SPI0_TX)
            pios |= 1u << (sel >> 3);
    }
    for (int i = 0; i < 3; i++)
        if (pios & (1u << i))
            pio_sync(m, &m->pio[i]);
}

uint32_t dma_read(chip_t *m, uint32_t off, bool peek)
{
    dma_t *d = &m->dma;
    if (!peek)
        sync_pacers(m);
    if (off < 0x400) {
        dma_ch_t *c = &d->ch[off >> 6];
        switch (off & 0x3f) {
        case 0x00:
        case 0x14:
        case 0x28:
        case 0x3c: return c->read_addr;
        case 0x04:
        case 0x18:
        case 0x2c:
        case 0x34: return c->write_addr;
        case 0x08:
        case 0x1c:
        case 0x24:
        case 0x38: return c->trans_count;
        default: return c->ctrl;
        }
    }
    switch (off) {
    case 0x400: return d->intr;
    case 0x404:
    case 0x414:
    case 0x424:
    case 0x434: return d->inte[(off - 0x404) >> 4];
    case 0x408:
    case 0x418:
    case 0x428:
    case 0x438: return d->intf[(off - 0x408) >> 4];
    case 0x40c:
    case 0x41c:
    case 0x42c:
    case 0x43c: {
        int k = (int)((off - 0x40c) >> 4);
        return (d->intr & d->inte[k]) | d->intf[k];
    }
    case 0x440:
    case 0x444:
    case 0x448:
    case 0x44c: return d->timer[(off - 0x440) >> 2];
    case 0x454: return d->sniff_ctrl;
    case 0x458: return d->sniff_data;
    case 0x464: return 0;
    case 0x468: return 16;
    default:
        if (off >= 0x480 && off < 0x4d4)
            return d->seccfg[((off - 0x480) >> 2) & 15];
        if (off >= 0x800 && off < 0xc00) {
            dma_ch_t *c = &d->ch[(off - 0x800) >> 6];
            return (off & 4) ? c->count_reload : 0;
        }
        return 0;
    }
}

void dma_write(chip_t *m, uint32_t off, uint32_t v)
{
    dma_t *d = &m->dma;
    if (off < 0x400) {
        int ch = (int)(off >> 6);
        dma_ch_t *c = &d->ch[ch];
        bool trig = false;
        switch (off & 0x3f) {
        case 0x00:
        case 0x14:
        case 0x28: c->read_addr = v; break;
        case 0x3c: c->read_addr = v; trig = true; break;
        case 0x04:
        case 0x18:
        case 0x34: c->write_addr = v; break;
        case 0x2c: c->write_addr = v; trig = true; break;
        case 0x08:
        case 0x24:
        case 0x38: c->count_reload = v; break;
        case 0x1c: c->count_reload = v; trig = true; break;
        case 0x0c:
            c->ctrl = (c->ctrl & (CH_BUSY | 0xe0000000u)) | (v & 0x03ffffffu);
            c->ctrl &= ~(v & 0x60000000u);
            trig = true;
            break;
        default:
            c->ctrl = (c->ctrl & (CH_BUSY | 0xe0000000u)) | (v & 0x03ffffffu);
            break;
        }
        if (trig && v)
            trigger(m, ch);
        else if (trig && (off & 0x3f) == 0x0c)
            trigger(m, ch);
        timers_arm(m);
        dma_kick(m);
        return;
    }
    switch (off) {
    case 0x400:
    case 0x40c:
    case 0x41c:
    case 0x42c:
    case 0x43c: d->intr &= ~v; break;
    case 0x404:
    case 0x414:
    case 0x424:
    case 0x434: d->inte[(off - 0x404) >> 4] = v & 0xffff; break;
    case 0x408:
    case 0x418:
    case 0x428:
    case 0x438: d->intf[(off - 0x408) >> 4] = v & 0xffff; break;
    case 0x440:
    case 0x444:
    case 0x448:
    case 0x44c: d->timer[(off - 0x440) >> 2] = v; break;
    case 0x450:
        for (int ch = 0; ch < 16; ch++)
            if (v & (1u << ch))
                trigger(m, ch);
        timers_arm(m);
        dma_kick(m);
        break;
    case 0x454: d->sniff_ctrl = v; break;
    case 0x458: d->sniff_data = v; break;
    case 0x464:
        for (int ch = 0; ch < 16; ch++)
            if (v & (1u << ch))
                d->ch[ch].ctrl &= ~CH_BUSY;
        break;
    default:
        if (off >= 0x480 && off < 0x4d4)
            d->seccfg[((off - 0x480) >> 2) & 15] = v;
        break;
    }
    update_irq(m);
}
