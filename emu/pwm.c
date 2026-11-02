#include <string.h>

#include "chip.h"

// PWM slices: counters advance at clk_sys divided by DIV, and a wrap raises the slice's interrupt
// and DREQ. Wraps are scheduled only while an interrupt or DMA is listening for them.

static emu_event_t wrap_ev[12];

void pwm_reset(pwm_t *p)
{
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < 12; i++) {
        p->s[i].div = 1u << 4;
        p->s[i].top = 0xffff;
    }
}

static uint64_t tick_ps(chip_t *m, pwm_slice_t *s)
{
    uint32_t i = (s->div >> 4) & 0xff, f = s->div & 0xf;
    if (!i)
        i = 256;
    return (uint64_t)m->sys_ps * (i * 16 + f) / 16;
}

static uint64_t period_ticks(pwm_slice_t *s)
{
    uint64_t t = (uint64_t)(s->top & 0xffff) + 1;
    return (s->csr & 2) ? 2 * t : t;
}

static uint32_t counter(chip_t *m, pwm_slice_t *s, int idx)
{
    if (!(m->pwm.en & (1u << idx)))
        return s->ctr;
    uint64_t ticks = (m->now - s->t0) / tick_ps(m, s);
    return (uint32_t)((s->ctr + ticks) % period_ticks(s));
}

static void update_irq(chip_t *m)
{
    pwm_t *p = &m->pwm;
    for (int k = 0; k < 2; k++)
        chip_irq(m, IRQ_PWM_WRAP_0 + (unsigned)k, ((p->intr & p->inte[k]) | p->intf[k]) != 0);
}

static void schedule_wrap(chip_t *m, int idx);

static void wrap_fire(chip_t *m, emu_event_t *ev)
{
    int idx = (int)(ev - wrap_ev);
    m->pwm.intr |= 1u << idx;
    update_irq(m);
    chip_dreq(m, DREQ_PWM_WRAP0 + (unsigned)idx, true);
    chip_dreq(m, DREQ_PWM_WRAP0 + (unsigned)idx, false);
    schedule_wrap(m, idx);
}

static void schedule_wrap(chip_t *m, int idx)
{
    pwm_t *p = &m->pwm;
    pwm_slice_t *s = &p->s[idx];
    ev_cancel(m, &wrap_ev[idx]);
    bool listening = ((p->inte[0] | p->inte[1]) & (1u << idx)) != 0;
    for (int ch = 0; ch < 16 && !listening; ch++)
        if ((m->dma.ch[ch].ctrl & (1u << 26)) && ((m->dma.ch[ch].ctrl >> 17) & 0x3f) == DREQ_PWM_WRAP0 + (unsigned)idx)
            listening = true;
    if (!(p->en & (1u << idx)) || !listening)
        return;
    uint64_t per = period_ticks(s);
    uint32_t c = counter(m, s, idx);
    uint64_t remaining = per - c;
    wrap_ev[idx].fn = wrap_fire;
    ev_schedule(m, &wrap_ev[idx], m->now + remaining * tick_ps(m, s));
}

static void rebase(chip_t *m, int idx)
{
    pwm_slice_t *s = &m->pwm.s[idx];
    s->ctr = counter(m, s, idx);
    s->t0 = m->now;
}

uint32_t pwm_read(chip_t *m, uint32_t off)
{
    pwm_t *p = &m->pwm;
    if (off < 0xf0) {
        int idx = (int)(off / 0x14);
        pwm_slice_t *s = &p->s[idx];
        switch (off % 0x14) {
        case 0x00: return s->csr;
        case 0x04: return s->div;
        case 0x08: return counter(m, s, idx);
        case 0x0c: return s->cc;
        default: return s->top;
        }
    }
    switch (off) {
    case 0xf0: return p->en;
    case 0xf4: return p->intr;
    case 0xf8: return p->inte[0];
    case 0xfc: return p->intf[0];
    case 0x100: return (p->intr & p->inte[0]) | p->intf[0];
    case 0x104: return p->inte[1];
    case 0x108: return p->intf[1];
    case 0x10c: return (p->intr & p->inte[1]) | p->intf[1];
    default: return 0;
    }
}

void pwm_write(chip_t *m, uint32_t off, uint32_t v)
{
    pwm_t *p = &m->pwm;
    if (off < 0xf0) {
        int idx = (int)(off / 0x14);
        pwm_slice_t *s = &p->s[idx];
        rebase(m, idx);
        switch (off % 0x14) {
        case 0x00:
            s->csr = v & 0xff;
            if (v & 1)
                p->en |= 1u << idx;
            else
                p->en &= ~(1u << idx);
            break;
        case 0x04: s->div = v & 0xfff; break;
        case 0x08: s->ctr = v & 0xffff; break;
        case 0x0c: s->cc = v; break;
        default: s->top = v & 0xffff; break;
        }
        schedule_wrap(m, idx);
        gpio_refresh(m);
        return;
    }
    switch (off) {
    case 0xf0:
        for (int i = 0; i < 12; i++)
            rebase(m, i);
        p->en = v & 0xfff;
        for (int i = 0; i < 12; i++) {
            if (v & (1u << i))
                p->s[i].csr |= 1;
            else
                p->s[i].csr &= ~1u;
            schedule_wrap(m, i);
        }
        break;
    case 0xf4: p->intr &= ~v; break;
    case 0xf8: p->inte[0] = v; break;
    case 0xfc: p->intf[0] = v; break;
    case 0x104: p->inte[1] = v; break;
    case 0x108: p->intf[1] = v; break;
    default: break;
    }
    for (int i = 0; i < 12; i++)
        schedule_wrap(m, i);
    update_irq(m);
}
