#include <string.h>

#include "chip.h"

// Synopsys DesignWare I2C in master mode. Each queued command takes nine SCL periods (plus an
// address phase after START or RESTART) at the rate the driver programmed, so polled transfers
// take as long as on the chip. Devices see whole write bursts and are asked for read bytes as the
// read phase begins.

#define R_TX_ABRT (1u << 6)
#define R_TX_EMPTY (1u << 4)
#define R_RX_FULL (1u << 2)
#define R_RX_OVER (1u << 1)
#define R_ACTIVITY (1u << 8)
#define R_STOP_DET (1u << 9)
#define R_START_DET (1u << 10)

extern bool g_fast_bus;

typedef struct {
    uint16_t cmd[16];
    int head, count;
} i2c_txq_t;

static i2c_txq_t txq[2];
static bool tx_flushed[2];

void dw_i2c_reset(dw_i2c_t *s, int idx)
{
    memset(s, 0, sizeof(*s));
    s->idx = idx;
    s->con = 0x65;
    s->tar = 0x55;
    s->sar = 0x55;
    s->ss_hcnt = 0x28;
    s->ss_lcnt = 0x2f;
    s->fs_hcnt = 0x06;
    s->fs_lcnt = 0x0d;
    s->sda_hold = 1;
    s->fs_spklen = 7;
    s->sda_setup = 0x64;
    s->ack_general_call = 1;
    memset(&txq[idx], 0, sizeof(txq[idx]));
    tx_flushed[idx] = false;
}

// The last period worked out for each controller, as drivers poll the status in a tight loop.
static struct {
    uint32_t clocks;
    uint64_t hz, ps;
} scl_memo[2];

static uint64_t scl_period(chip_t *m, dw_i2c_t *s)
{
    if (g_fast_bus || !m->sys_hz)
        return 0;
    uint32_t h, l;
    if (((s->con >> 1) & 3) == 1) {
        h = s->ss_hcnt;
        l = s->ss_lcnt;
    } else {
        h = s->fs_hcnt;
        l = s->fs_lcnt;
    }
    uint32_t clocks = h + l + 2 * s->fs_spklen;
    if (scl_memo[s->idx].clocks != clocks || scl_memo[s->idx].hz != m->sys_hz) {
        scl_memo[s->idx].clocks = clocks;
        scl_memo[s->idx].hz = m->sys_hz;
        scl_memo[s->idx].ps = (uint64_t)clocks * PS_PER_S / m->sys_hz;
    }
    return scl_memo[s->idx].ps;
}

static void update_irq(chip_t *m, dw_i2c_t *s)
{
    i2c_txq_t *q = &txq[s->idx];
    s->raw_intr &= ~(R_TX_EMPTY | R_RX_FULL | R_ACTIVITY);
    if ((int)q->count <= (int)s->tx_tl && (q->count == 0 || !(s->con & (1u << 8))))
        s->raw_intr |= R_TX_EMPTY;
    if (s->rxn > (int)s->rx_tl)
        s->raw_intr |= R_RX_FULL;
    if (s->active || q->count)
        s->raw_intr |= R_ACTIVITY;
    chip_irq(m, IRQ_I2C0 + (unsigned)s->idx, (s->raw_intr & s->intr_mask) != 0);
    chip_dreq(m, DREQ_I2C0_TX + 2u * (unsigned)s->idx, (s->dma_cr & 2) && q->count <= (int)s->dma_tdlr);
    chip_dreq(m, DREQ_I2C0_RX + 2u * (unsigned)s->idx, (s->dma_cr & 1) && s->rxn > (int)s->dma_rdlr);
}

static uint64_t cmd_duration(chip_t *m, dw_i2c_t *s, uint16_t cmd)
{
    uint64_t p = scl_period(m, s);
    bool read = cmd & 0x100, restart = cmd & 0x400;
    uint64_t t = 9 * p;
    if (!s->active || restart || read != s->in_read)
        t += 10 * p;
    if (cmd & 0x200)
        t += p;
    return t;
}

static void flush_write(dw_i2c_t *s, bool nostop)
{
    if (s->dev && s->wlen)
        devices_i2c_write(s->dev, s->wbuf, s->wlen, nostop);
    s->wlen = 0;
}

static void abort_transfer(chip_t *m, dw_i2c_t *s, uint32_t source)
{
    s->abrt_source |= source;
    s->raw_intr |= R_TX_ABRT | R_STOP_DET;
    txq[s->idx].count = 0;
    tx_flushed[s->idx] = true;
    s->active = false;
    s->wlen = 0;
    s->dev = NULL;
    (void)m;
}

static void process_cmd(chip_t *m, dw_i2c_t *s, uint16_t cmd)
{
    bool read = cmd & 0x100, stop = cmd & 0x200, restart = cmd & 0x400;
    if (!s->active || restart || read != s->in_read) {
        if (s->active && !s->in_read)
            flush_write(s, true);
        void *dev = NULL;
        if (!devices_i2c_start(m, s->idx, (uint8_t)(s->tar & 0x7f), &dev)) {
            abort_transfer(m, s, 1);
            return;
        }
        s->dev = dev;
        s->active = true;
        s->in_read = read;
        s->raw_intr |= R_START_DET;
        if (read) {
            memset(s->rbuf, 0xff, sizeof(s->rbuf));
            devices_i2c_read(dev, s->rbuf, (int)sizeof(s->rbuf), true);
            s->rpos = 0;
        }
    }
    if (read) {
        uint8_t b = s->rpos < (int)sizeof(s->rbuf) ? s->rbuf[s->rpos++] : 0xff;
        if (s->rxn < 16) {
            s->rxf[(s->rxh + s->rxn) & 15] = b;
            s->rxn++;
        } else {
            s->raw_intr |= R_RX_OVER;
        }
    } else if (s->wlen < (int)sizeof(s->wbuf)) {
        s->wbuf[s->wlen++] = (uint8_t)cmd;
    }
    if (stop) {
        if (!s->in_read)
            flush_write(s, false);
        s->active = false;
        s->dev = NULL;
        s->raw_intr |= R_STOP_DET;
    }
}

static void i2c_event(chip_t *m, emu_event_t *ev);

static void sync(chip_t *m, dw_i2c_t *s)
{
    i2c_txq_t *q = &txq[s->idx];
    bool any = false;
    while (q->count > 0 && (s->enable & 1)) {
        uint16_t cmd = q->cmd[q->head];
        uint64_t d = cmd_duration(m, s, cmd);
        if (s->busy_until + d > m->now)
            break;
        s->busy_until += d;
        q->head = (q->head + 1) & 15;
        q->count--;
        process_cmd(m, s, cmd);
        any = true;
    }
    if (q->count == 0 && s->busy_until < m->now)
        s->busy_until = m->now;
    if (any)
        update_irq(m, s);
    if (q->count > 0 && (s->intr_mask || s->dma_cr)) {
        s->ev.fn = i2c_event;
        s->ev.ctx = s;
        ev_schedule(m, &s->ev, s->busy_until + cmd_duration(m, s, q->cmd[q->head]));
    } else {
        ev_cancel(m, &s->ev);
    }
}

static void i2c_event(chip_t *m, emu_event_t *ev)
{
    sync(m, ev->ctx);
}

uint32_t dw_i2c_read(chip_t *m, dw_i2c_t *s, uint32_t off, bool peek)
{
    if (!peek)
        sync(m, s);
    i2c_txq_t *q = &txq[s->idx];
    uint32_t v;
    switch (off) {
    case 0x00: return s->con;
    case 0x04: return s->tar;
    case 0x08: return s->sar;
    case 0x10:
        if (!s->rxn)
            return 0;
        v = s->rxf[s->rxh];
        if (!peek) {
            s->rxh = (s->rxh + 1) & 15;
            s->rxn--;
            update_irq(m, s);
        }
        return v;
    case 0x14: return s->ss_hcnt;
    case 0x18: return s->ss_lcnt;
    case 0x1c: return s->fs_hcnt;
    case 0x20: return s->fs_lcnt;
    case 0x2c: return s->raw_intr & s->intr_mask;
    case 0x30: return s->intr_mask;
    case 0x34: return s->raw_intr;
    case 0x38: return s->rx_tl;
    case 0x3c: return s->tx_tl;
    case 0x40:
        if (!peek) {
            s->raw_intr &= ~0x1febu;
            s->abrt_source = 0;
            tx_flushed[s->idx] = false;
            update_irq(m, s);
        }
        return 0;
    case 0x44:
    case 0x48:
    case 0x4c:
    case 0x50:
    case 0x54:
    case 0x58:
    case 0x5c:
    case 0x60:
    case 0x64:
    case 0x68:
    case 0xa8:
        if (!peek) {
            static const uint32_t bits[] = {1u << 0, 1u << 1, 1u << 3, 1u << 5, 1u << 6, 1u << 7, 1u << 8, 1u << 9,
                                            1u << 10, 1u << 11};
            if (off == 0xa8) {
                s->raw_intr &= ~(1u << 12);
            } else {
                s->raw_intr &= ~bits[(off - 0x44) >> 2];
                if (off == 0x54) {
                    s->abrt_source = 0;
                    tx_flushed[s->idx] = false;
                }
            }
            update_irq(m, s);
        }
        return 0;
    case 0x6c: return s->enable;
    case 0x70:
        return ((s->active || q->count) ? 1u : 0) | (q->count < 16 ? 2u : 0) | (q->count == 0 ? 4u : 0) |
               (s->rxn ? 8u : 0) | (s->rxn == 16 ? 16u : 0) | ((s->active || q->count) ? 32u : 0);
    case 0x74: return (uint32_t)q->count;
    case 0x78: return (uint32_t)s->rxn;
    case 0x7c: return s->sda_hold;
    case 0x80: return s->abrt_source;
    case 0x84: return s->slv_data_nack;
    case 0x88: return s->dma_cr;
    case 0x8c: return s->dma_tdlr;
    case 0x90: return s->dma_rdlr;
    case 0x94: return s->sda_setup;
    case 0x98: return s->ack_general_call;
    case 0x9c: return s->enable & 1;
    case 0xa0: return s->fs_spklen;
    case 0xf4: return 0x00ffff6eu;
    case 0xf8: return 0x3230312au;
    case 0xfc: return 0x44570140u;
    default: return 0;
    }
}

void dw_i2c_write(chip_t *m, dw_i2c_t *s, uint32_t off, uint32_t v)
{
    sync(m, s);
    i2c_txq_t *q = &txq[s->idx];
    switch (off) {
    case 0x00: s->con = v & 0x7ff; break;
    case 0x04: s->tar = v & 0xfff; break;
    case 0x08: s->sar = v & 0x3ff; break;
    case 0x10:
        if (tx_flushed[s->idx])
            break;
        if (q->count < 16) {
            if (q->count == 0 && s->busy_until < m->now)
                s->busy_until = m->now;
            q->cmd[(q->head + q->count) & 15] = (uint16_t)(v & 0x7ff);
            q->count++;
        } else {
            s->raw_intr |= 1u << 3;
        }
        break;
    case 0x14: s->ss_hcnt = v & 0xffff; break;
    case 0x18: s->ss_lcnt = v & 0xffff; break;
    case 0x1c: s->fs_hcnt = v & 0xffff; break;
    case 0x20: s->fs_lcnt = v & 0xffff; break;
    case 0x30: s->intr_mask = v & 0x1fff; break;
    case 0x38: s->rx_tl = v & 0xff; break;
    case 0x3c: s->tx_tl = v & 0xff; break;
    case 0x6c:
        if (!(v & 1)) {
            q->count = 0;
            s->rxn = 0;
            s->active = false;
            s->wlen = 0;
            s->dev = NULL;
        }
        s->enable = v & 7;
        break;
    case 0x7c: s->sda_hold = v; break;
    case 0x84: s->slv_data_nack = v & 1; break;
    case 0x88: s->dma_cr = v & 3; break;
    case 0x8c: s->dma_tdlr = v & 0xf; break;
    case 0x90: s->dma_rdlr = v & 0xf; break;
    case 0x94: s->sda_setup = v & 0xff; break;
    case 0x98: s->ack_general_call = v & 1; break;
    case 0xa0: s->fs_spklen = v & 0xff; break;
    default: break;
    }
    update_irq(m, s);
    sync(m, s);
}
