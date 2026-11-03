#include <string.h>

#include "chip.h"

// TIMER0/1 (64-bit microsecond counters with four alarms each), the watchdog, and the always-on
// timer and scratch state in POWMAN.

static uint64_t tick_ps(chip_t *m, unsigned gen)
{
    uint32_t ctrl = m->ticks[gen * 3], cycles = m->ticks[gen * 3 + 1] & 0x1ff;
    if (!(ctrl & 1) || !cycles || !m->ref_hz)
        return 0;
    if (m->tick_memo[gen].cycles != cycles || m->tick_memo[gen].ref_hz != m->ref_hz) {
        m->tick_memo[gen].cycles = cycles;
        m->tick_memo[gen].ref_hz = m->ref_hz;
        m->tick_memo[gen].ps = (uint64_t)cycles * PS_PER_S / m->ref_hz;
    }
    return m->tick_memo[gen].ps;
}

static uint64_t timer_period(chip_t *m, timer_t_ *t)
{
    if (t->source & 1)
        return m->sys_ps;
    return tick_ps(m, 2 + (unsigned)t->idx);
}

uint64_t timer_us(chip_t *m, timer_t_ *t)
{
    uint64_t p = timer_period(m, t);
    if (!p || (t->pause & 1))
        return t->base_us;
    uint64_t dt = m->now - t->base_t;
    // Firmware polls the timer many times per tick, and a 64-bit divide is slow.
    if (p != t->q_period || t->base_t != t->q_base || dt < t->q_dt || dt - t->q_dt >= p) {
        t->q = dt / p;
        t->q_dt = t->q * p;
        t->q_period = p;
        t->q_base = t->base_t;
    }
    return t->base_us + t->q;
}

static void timer_irqs(chip_t *m, timer_t_ *t)
{
    uint32_t ints = (t->intr | t->intf) & t->inte;
    for (int i = 0; i < 4; i++)
        chip_irq(m, (unsigned)(t->idx * 4 + i), (ints >> i) & 1);
}

static void alarm_fire(chip_t *m, emu_event_t *ev)
{
    timer_t_ *t = ev->ctx;
    int i = (int)(ev - t->ev);
    if (!(t->armed & (1u << i)))
        return;
    // An alarm compares against the low 32 bits each tick; a retime may have moved the deadline.
    if ((uint32_t)timer_us(m, t) != t->alarm[i] && (int32_t)((uint32_t)timer_us(m, t) - t->alarm[i]) < 0)
        return;
    t->armed &= ~(1u << i);
    t->intr |= 1u << i;
    timer_irqs(m, t);
}

static void alarm_schedule(chip_t *m, timer_t_ *t, int i)
{
    ev_cancel(m, &t->ev[i]);
    if (!(t->armed & (1u << i)))
        return;
    uint64_t p = timer_period(m, t);
    if (!p || (t->pause & 1))
        return;
    uint32_t now_lo = (uint32_t)timer_us(m, t);
    uint32_t delta = t->alarm[i] - now_lo;
    uint64_t elapsed = (m->now - t->base_t) % p;
    uint64_t when = m->now + (uint64_t)delta * p - elapsed;
    if (when < m->now)
        when = m->now;
    t->ev[i].fn = alarm_fire;
    t->ev[i].ctx = t;
    ev_schedule(m, &t->ev[i], when);
}

void timer_retime(chip_t *m)
{
    for (int k = 0; k < 2; k++) {
        timer_t_ *t = &m->timer[k];
        uint64_t v = timer_us(m, t);
        t->base_us = v;
        t->base_t = m->now;
        for (int i = 0; i < 4; i++)
            alarm_schedule(m, t, i);
    }
}

void timer_reset(chip_t *m, timer_t_ *t, int idx)
{
    for (int i = 0; i < 4; i++)
        ev_cancel(m, &t->ev[i]);
    memset(t, 0, sizeof(*t));
    t->idx = idx;
    t->base_t = m->now;
    t->dbgpause = 7;
    for (int i = 0; i < 4; i++)
        t->ev[i].ctx = t;
    for (int i = 0; i < 4; i++)
        chip_irq(m, (unsigned)(idx * 4 + i), false);
}

uint32_t timer_read(chip_t *m, timer_t_ *t, uint32_t off, bool peek)
{
    uint64_t now = timer_us(m, t);
    switch (off) {
    case 0x08: return t->latch_hi;
    case 0x0c:
        if (!peek)
            t->latch_hi = (uint32_t)(now >> 32);
        return (uint32_t)now;
    case 0x10:
    case 0x14:
    case 0x18:
    case 0x1c: return t->alarm[(off - 0x10) >> 2];
    case 0x20: return t->armed;
    case 0x24: return (uint32_t)(now >> 32);
    case 0x28: return (uint32_t)now;
    case 0x2c: return t->dbgpause;
    case 0x30: return t->pause;
    case 0x34: return t->locked;
    case 0x38: return t->source;
    case 0x3c: return t->intr;
    case 0x40: return t->inte;
    case 0x44: return t->intf;
    case 0x48: return (t->intr | t->intf) & t->inte;
    default: return 0;
    }
}

void timer_write(chip_t *m, timer_t_ *t, uint32_t off, uint32_t v)
{
    if ((t->locked & 1) && off != 0x3c)
        return;
    switch (off) {
    case 0x00:
        t->base_us = ((uint64_t)v << 32) | t->latch_hi;
        t->base_t = m->now;
        for (int i = 0; i < 4; i++)
            alarm_schedule(m, t, i);
        break;
    case 0x04:
        t->latch_hi = v;
        break;
    case 0x10:
    case 0x14:
    case 0x18:
    case 0x1c: {
        int i = (int)((off - 0x10) >> 2);
        t->alarm[i] = v;
        t->armed |= 1u << i;
        alarm_schedule(m, t, i);
        break;
    }
    case 0x20:
        t->armed &= ~(v & 0xf);
        for (int i = 0; i < 4; i++)
            if (v & (1u << i))
                ev_cancel(m, &t->ev[i]);
        break;
    case 0x2c: t->dbgpause = v & 6; break;
    case 0x30:
        t->base_us = timer_us(m, t);
        t->base_t = m->now;
        t->pause = v & 1;
        for (int i = 0; i < 4; i++)
            alarm_schedule(m, t, i);
        break;
    case 0x34: t->locked |= v & 1; break;
    case 0x38:
        t->base_us = timer_us(m, t);
        t->base_t = m->now;
        t->source = v & 1;
        for (int i = 0; i < 4; i++)
            alarm_schedule(m, t, i);
        break;
    case 0x3c:
        t->intr &= ~v;
        timer_irqs(m, t);
        break;
    case 0x40:
        t->inte = v & 0xf;
        timer_irqs(m, t);
        break;
    case 0x44:
        t->intf = v & 0xf;
        timer_irqs(m, t);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------------------------
// Watchdog

static uint32_t wd_remaining(chip_t *m)
{
    if (!(m->wd_ctrl & (1u << 30)))
        return m->wd_load;
    uint64_t p = tick_ps(m, 4);
    if (!p)
        return m->wd_load;
    uint64_t ticks = (m->now - m->wd_t0) / p;
    return ticks >= m->wd_load ? 0 : (uint32_t)(m->wd_load - ticks);
}

static void wd_fire(chip_t *m, emu_event_t *ev)
{
    (void)ev;
    if (!(m->wd_ctrl & (1u << 30)) || wd_remaining(m) != 0)
        return;
    m->wd_reason = 1;
    chip_request(m, REQ_RESET, "watchdog");
}

void watchdog_retime(chip_t *m)
{
    uint32_t rem = wd_remaining(m);
    m->wd_load = rem;
    m->wd_t0 = m->now;
    ev_cancel(m, &m->wd_ev);
    uint64_t p = tick_ps(m, 4);
    if ((m->wd_ctrl & (1u << 30)) && p) {
        m->wd_ev.fn = wd_fire;
        ev_schedule(m, &m->wd_ev, m->now + (uint64_t)rem * p);
    }
}

uint32_t watchdog_read(chip_t *m, uint32_t off)
{
    switch (off) {
    case 0x00: return (m->wd_ctrl & ~0x00ffffffu) | (wd_remaining(m) & 0xffffff);
    case 0x08: return m->wd_reason;
    default:
        if (off >= 0x0c && off <= 0x28)
            return m->wd_scratch[(off - 0x0c) >> 2];
        return 0;
    }
}

void watchdog_write(chip_t *m, uint32_t off, uint32_t v)
{
    switch (off) {
    case 0x00:
        if (v & (1u << 31)) {
            m->wd_reason = 2;
            chip_request(m, REQ_RESET, "watchdog trigger");
        }
        m->wd_load = wd_remaining(m);
        m->wd_t0 = m->now;
        m->wd_ctrl = v & 0x47000000u;
        watchdog_retime(m);
        break;
    case 0x04:
        m->wd_load = v & 0xffffff;
        m->wd_t0 = m->now;
        watchdog_retime(m);
        break;
    default:
        if (off >= 0x0c && off <= 0x28)
            m->wd_scratch[(off - 0x0c) >> 2] = v;
        break;
    }
}

// ---------------------------------------------------------------------------------------------
// POWMAN

static uint64_t aon_ms(chip_t *m)
{
    if (!(m->powman[0x88 >> 2] & 2))
        return m->aon_base_ms;
    return m->aon_base_ms + (m->now - m->aon_base_t) / PS_PER_MS;
}

uint32_t powman_read(chip_t *m, uint32_t off)
{
    switch (off) {
    case 0x08: return 0x10;
    case 0x0c: return m->powman[3] & ~0x8000u;
    case 0x70: return (uint32_t)(aon_ms(m) >> 32);
    case 0x74: return (uint32_t)aon_ms(m);
    case 0x88: {
        uint32_t v = m->powman[0x88 >> 2];
        if (v & 2)
            v |= (v & 0x200) ? 0x10000 : 0x20000;
        return v;
    }
    default: return off < 0x100 ? m->powman[off >> 2] : 0;
    }
}

void powman_write(chip_t *m, uint32_t off, uint32_t v)
{
    if ((v >> 16) != 0x5afe) {
        m->powman[0] = 1;
        return;
    }
    v &= 0xffff;
    switch (off) {
    case 0x00: m->powman[0] = 0; break;
    case 0x2c: m->powman[off >> 2] = (m->powman[off >> 2] & ~1u) | (v & 1); break;
    case 0x6c: {
        uint64_t t = ((uint64_t)m->powman[0x60 >> 2] << 48) | ((uint64_t)m->powman[0x64 >> 2] << 32) |
                     ((uint64_t)m->powman[0x68 >> 2] << 16) | v;
        m->powman[off >> 2] = v;
        m->aon_base_ms = t;
        m->aon_base_t = m->now;
        break;
    }
    case 0x88: {
        uint32_t old = m->powman[off >> 2];
        if (v & 4) {
            m->aon_base_ms = 0;
            m->aon_base_t = m->now;
        }
        if ((v & 2) && !(old & 2))
            m->aon_base_t = m->now;
        else if (!(v & 2) && (old & 2))
            m->aon_base_ms = aon_ms(m);
        m->powman[off >> 2] = v & ~4u;
        break;
    }
    default:
        if (off < 0x100)
            m->powman[off >> 2] = v;
        break;
    }
}
