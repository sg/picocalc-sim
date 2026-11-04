#include <stdlib.h>
#include <string.h>

#include "chip.h"
#include "pcsim/bus.h"
#include "pcsim/sim.h"

// Records the waveform of the pins a device listens to without feeding anything back into the
// chip: a PWM output as its slice settings, so a fast carrier costs nothing per cycle, and any
// other driver as edges at the time the core or PIO state machine made them.

#define MAX_PINS 4
#define SEGS 16384
#define BATCH 512
// PIO state machines catch up with the present at least every 5 us and the cores are at most a
// chunk apart, so a window this far back has seen all of its edges.
#define LAG_PS (50 * PS_PER_US)
// A PWM carrier above 20 kHz is inaudible; counting its partial periods would only alias it into
// the band.
#define SMOOTH_PS (50 * PS_PER_US)

typedef struct {
    uint64_t t;
    pin_wave_t w;
} seg_t;

typedef struct {
    unsigned pin;
    // seg[tail] is the waveform at done_t; later ones start where the next begins.
    uint32_t head, tail;
    seg_t seg[SEGS];
} track_t;

uint64_t g_wave_pins;

static struct {
    int n;
    track_t *tr[MAX_PINS];
    unsigned rate;
    uint64_t base, done, done_t;
    pcsim_pin_wave_fn fn;
    void *ctx;
    bool overflowed;
    float buf[BATCH * MAX_PINS];
} W;

static bool wave_eq(const pin_wave_t *a, const pin_wave_t *b)
{
    if (a->kind != b->kind || a->level != b->level)
        return false;
    if (a->kind == WAVE_LEVEL)
        return true;
    return a->phase_correct == b->phase_correct && a->ctr == b->ctr && a->period == b->period &&
           a->high == b->high && a->t0 == b->t0 && a->tick == b->tick;
}

static void append(track_t *tr, uint64_t t, const pin_wave_t *w)
{
    seg_t *last = &tr->seg[(tr->head - 1) & (SEGS - 1)];
    if (wave_eq(&last->w, w))
        return;
    if (t < W.done_t)
        t = W.done_t;
    if (t < last->t)
        t = last->t;
    if (t == last->t || tr->head - tr->tail == SEGS) {
        if (t != last->t && !W.overflowed) {
            W.overflowed = true;
            pcsim_log("pin %u changes faster than its waveform can be recorded; some are merged", tr->pin);
        }
        last->w = *w;
        return;
    }
    tr->seg[tr->head & (SEGS - 1)] = (seg_t){t, *w};
    tr->head++;
}

void pinwave_update(chip_t *m, uint64_t pins)
{
    if (!W.n)
        return;
    uint64_t t = m->edge_t ? m->edge_t : m->now;
    for (int i = 0; i < W.n; i++) {
        track_t *tr = W.tr[i];
        if (!((pins >> tr->pin) & 1))
            continue;
        pin_wave_t w;
        gpio_wave(m, tr->pin, &w);
        append(tr, t, &w);
    }
}

// Time a running PWM channel has spent high since its count was at zero, up to t.
static uint64_t pwm_high_to(const pin_wave_t *w, uint64_t t)
{
    uint64_t e = t > w->t0 ? t - w->t0 : 0;
    uint64_t pos = w->ctr + e / w->tick, r = e % w->tick;
    uint64_t cycles = pos / w->period, x = pos % w->period;
    uint64_t h = w->high, per_cycle = h, f = x < h ? x : h;
    bool on = x < h;
    if (w->phase_correct) {
        uint64_t down = w->period - h;
        per_cycle = 2 * h;
        if (x >= down) {
            f += x - down;
            on = true;
        }
    }
    return (cycles * per_cycle + f) * w->tick + (on ? r : 0);
}

static uint64_t high_ps(const pin_wave_t *w, uint64_t a, uint64_t b)
{
    if (w->kind == WAVE_LEVEL)
        return w->level ? b - a : 0;
    uint64_t hi;
    if ((uint64_t)w->period * w->tick < SMOOTH_PS)
        hi = (b - a) * (w->phase_correct ? 2ull * w->high : w->high) / w->period;
    else
        hi = pwm_high_to(w, b) - pwm_high_to(w, a);
    return w->level ? (b - a) - hi : hi;
}

static uint64_t integrate(track_t *tr, uint64_t a, uint64_t b)
{
    uint64_t hi = 0, t = a;
    for (;;) {
        uint64_t next = tr->tail + 1 != tr->head ? tr->seg[(tr->tail + 1) & (SEGS - 1)].t : UINT64_MAX;
        uint64_t end = next < b ? next : b;
        if (end > t) {
            hi += high_ps(&tr->seg[tr->tail & (SEGS - 1)].w, t, end);
            t = end;
        }
        if (next > b)
            return hi;
        tr->tail++;
    }
}

static uint64_t window_end(uint64_t n)
{
    n++;
    return W.base + n / W.rate * PS_PER_S + n % W.rate * PS_PER_S / W.rate;
}

void pinwave_advance(chip_t *m)
{
    if (!W.n || m->now < LAG_PS)
        return;
    uint64_t horizon = m->now - LAG_PS;
    int k = 0;
    for (uint64_t end; (end = window_end(W.done)) <= horizon;) {
        double len = (double)(end - W.done_t);
        for (int i = 0; i < W.n; i++)
            W.buf[k * W.n + i] = (float)((double)integrate(W.tr[i], W.done_t, end) / len);
        W.done++;
        W.done_t = end;
        if (++k == BATCH) {
            W.fn(W.ctx, W.buf, (size_t)k);
            k = 0;
        }
    }
    if (k)
        W.fn(W.ctx, W.buf, (size_t)k);
}

void pinwave_stop(void)
{
    for (int i = 0; i < W.n; i++)
        free(W.tr[i]);
    memset(&W, 0, sizeof(W));
    g_wave_pins = 0;
}

bool pcsim_pin_wave(const unsigned *pins, int npins, unsigned rate_hz, pcsim_pin_wave_fn fn, void *ctx)
{
    chip_t *m = g_chip;
    if (!m || W.n || npins < 1 || npins > MAX_PINS || !rate_hz)
        return false;
    for (int i = 0; i < npins; i++)
        if (pins[i] >= m->board->num_gpio)
            return false;
    pcsim_machine_lock();
    for (int i = 0; i < npins; i++) {
        track_t *tr = calloc(1, sizeof(*tr));
        tr->pin = pins[i];
        tr->seg[0].t = m->now;
        gpio_wave(m, pins[i], &tr->seg[0].w);
        tr->head = 1;
        W.tr[i] = tr;
        g_wave_pins |= 1ull << pins[i];
    }
    W.rate = rate_hz;
    W.base = W.done_t = m->now;
    W.done = 0;
    W.fn = fn;
    W.ctx = ctx;
    W.n = npins;
    pcsim_machine_unlock();
    return true;
}
