#include <stdint.h>
#include <string.h>

#include "chip.h"

// The single-cycle IO block: GPIO output registers, inter-core FIFOs, spinlocks, doorbells,
// interpolators and the RISC-V platform timer, plus the Cortex-M33 GPIO coprocessor that reaches
// the same GPIO registers, with the RP2350's FIFOs, spinlocks and doorbells in a Secure and a
// Non-secure bank that each have their own interrupts (datasheet section 3.1.1).

void sio_fifo_irq(chip_t *m)
{
    for (int b = 0; b < (m->rp2040 ? 1 : 2); b++)
        for (int core = 0; core < 2; core++) {
            sio_fifo_t *rx = &m->sio[b].fifo[1 - core];
            sio_fifo_t *tx = &m->sio[b].fifo[core];
            bool level = rx->count > 0 || tx->wof || rx->roe;
            chip_irq_core(m, core, b ? IRQ_SIO_FIFO_NS : IRQ_SIO_FIFO, level);
        }
}

static void bell_irq(chip_t *m)
{
    for (int b = 0; b < (m->rp2040 ? 1 : 2); b++)
        for (int core = 0; core < 2; core++)
            chip_irq_core(m, core, b ? IRQ_SIO_BELL_NS : IRQ_SIO_BELL, m->sio[b].doorbell[core] != 0);
}

void sio_reset(chip_t *m)
{
    m->sio_out[0] = m->sio_out[1] = 0;
    m->sio_oe[0] = m->sio_oe[1] = 0;
    memset(m->sio, 0, sizeof(m->sio));
    memset(m->interp, 0, sizeof(m->interp));
    m->peri_nonsec = 0;
}

// The GPIOs ACCESSCTRL's GPIO_NSMASK registers grant Non-secure code, in the SIO's layout.
static uint32_t gpio_nsmask(const chip_t *m, int hi)
{
    return hi ? m->accessctrl[4] & 0xff00ffffu : m->accessctrl[3];
}

static inline uint32_t gpio_view(const chip_t *m, int b, int hi)
{
    return b ? gpio_nsmask(m, hi) : ~0u;
}

static uint32_t interp_lane(interp_t *ip, int lane, uint32_t *raw_out)
{
    uint32_t ctrl = ip->ctrl[lane];
    uint32_t in = (ctrl & (1u << 16)) ? ip->accum[1 - lane] : ip->accum[lane];
    unsigned shift = ctrl & 31, lsb = (ctrl >> 5) & 31, msb = (ctrl >> 10) & 31;
    uint32_t v = in >> shift;
    uint32_t mask = msb >= lsb ? (uint32_t)((((uint64_t)1 << (msb - lsb + 1)) - 1) << lsb) : 0;
    v &= mask;
    if ((ctrl & (1u << 15)) && msb < 31 && (v & (1u << msb)))
        v |= ~(uint32_t)((((uint64_t)1 << (msb + 1)) - 1));
    *raw_out = in;
    return v;
}

static void interp_results(interp_t *ip, uint32_t res[3])
{
    uint32_t raw0, raw1;
    uint32_t m0 = interp_lane(ip, 0, &raw0), m1 = interp_lane(ip, 1, &raw1);
    uint32_t a0 = (ip->ctrl[0] & (1u << 18)) ? raw0 : m0;
    uint32_t a1 = (ip->ctrl[1] & (1u << 18)) ? raw1 : m1;
    res[0] = ip->base[0] + a0;
    res[1] = ip->base[1] + a1;
    res[2] = ip->base[2] + m0 + m1;
    for (int i = 0; i < 2; i++) {
        uint32_t fm = (ip->ctrl[i] >> 19) & 3;
        res[i] |= fm << 28;
    }
}

static uint32_t interp_access(interp_t *ip, uint32_t off, bool write, uint32_t v)
{
    uint32_t res[3];
    switch (off) {
    case 0x00:
    case 0x04:
        if (write)
            ip->accum[off >> 2] = v;
        return ip->accum[off >> 2];
    case 0x08:
    case 0x0c:
    case 0x10:
        if (write)
            ip->base[(off - 0x08) >> 2] = v;
        return ip->base[(off - 0x08) >> 2];
    case 0x14:
    case 0x18:
    case 0x1c: {
        interp_results(ip, res);
        uint32_t r = res[(off - 0x14) >> 2];
        for (int i = 0; i < 2; i++)
            ip->accum[i] = (ip->ctrl[i] & (1u << 17)) ? res[1 - i] : res[i];
        return r;
    }
    case 0x20:
    case 0x24:
    case 0x28:
        interp_results(ip, res);
        return res[(off - 0x20) >> 2];
    case 0x2c:
    case 0x30:
        if (write)
            ip->ctrl[(off - 0x2c) >> 2] = v & 0x7fffff;
        return ip->ctrl[(off - 0x2c) >> 2];
    case 0x34:
    case 0x38:
        if (write)
            ip->accum[(off - 0x34) >> 2] += v;
        return 0;
    case 0x3c:
        if (write) {
            ip->base[0] = v & 0xffff;
            ip->base[1] = v >> 16;
        }
        return 0;
    default:
        return 0;
    }
}

static uint64_t mtime(chip_t *m)
{
    return m->now / PS_PER_US;
}

// The RP2040's divider, one per core: the M0+ has no divide instruction, so the SDK writes the
// operands here and reads the result eight cycles later.
static void div_calc(cpu_t *c, bool sign)
{
    __typeof__(c->m->rp.div[0]) *d = &c->m->rp.div[c->id];
    d->dirty = true;
    if (sign) {
        int32_t a = (int32_t)d->dividend, b = (int32_t)d->divisor;
        if (b == 0) {
            d->quotient = a < 0 ? 1u : 0xffffffffu;
            d->remainder = (uint32_t)a;
        } else if (a == INT32_MIN && b == -1) {
            d->quotient = (uint32_t)INT32_MIN;
            d->remainder = 0;
        } else {
            d->quotient = (uint32_t)(a / b);
            d->remainder = (uint32_t)(a % b);
        }
    } else if (d->divisor == 0) {
        d->quotient = 0xffffffffu;
        d->remainder = d->dividend;
    } else {
        d->quotient = d->dividend / d->divisor;
        d->remainder = d->dividend % d->divisor;
    }
}

static bool rp2040_read(cpu_t *c, uint32_t off, uint32_t *v)
{
    chip_t *m = c->m;
    __typeof__(m->rp.div[0]) *d = &m->rp.div[c->id];
    switch (off) {
    case 0x008: *v = rp2040_qspi_in(m); return true;
    case 0x010: *v = m->sio_out[0]; return true;
    case 0x020: *v = m->sio_oe[0]; return true;
    case 0x030: *v = m->rp.sio_hi_out; return true;
    case 0x040: *v = m->rp.sio_hi_oe; return true;
    case 0x060:
    case 0x068: *v = d->dividend; return true;
    case 0x064:
    case 0x06c: *v = d->divisor; return true;
    case 0x070:
        d->dirty = false;
        *v = d->quotient;
        return true;
    case 0x074: *v = d->remainder; return true;
    case 0x078: *v = 1u | (d->dirty ? 2u : 0); return true;
    default:
        if ((off >= 0x010 && off < 0x050) || off >= 0x180) {
            *v = 0;
            return true;
        }
        return false;
    }
}

static void apply_op(uint32_t *t, unsigned op, uint32_t v)
{
    switch (op) {
    case 0: *t = v; break;
    case 1: *t |= v; break;
    case 2: *t &= ~v; break;
    default: *t ^= v; break;
    }
}

static void gpio_regs_changed(cpu_t *c, uint32_t old_out[2], uint32_t old_oe[2]);

static bool rp2040_write(cpu_t *c, uint32_t off, uint32_t v)
{
    chip_t *m = c->m;
    __typeof__(m->rp.div[0]) *d = &m->rp.div[c->id];
    if (off >= 0x010 && off < 0x050) {
        unsigned reg = (off - 0x10) >> 2, grp = reg >> 2, op = reg & 3;
        if (grp < 2) {
            uint32_t old_out[2] = {m->sio_out[0], m->sio_out[1]}, old_oe[2] = {m->sio_oe[0], m->sio_oe[1]};
            apply_op(grp ? &m->sio_oe[0] : &m->sio_out[0], op, v & 0x3fffffffu);
            gpio_regs_changed(c, old_out, old_oe);
        } else {
            apply_op(grp == 3 ? &m->rp.sio_hi_oe : &m->rp.sio_hi_out, op, v & 0x3f);
            rp2040_sio_hi_changed(m);
        }
        return true;
    }
    switch (off) {
    case 0x060: d->dividend = v; div_calc(c, false); return true;
    case 0x064: d->divisor = v; div_calc(c, false); return true;
    case 0x068: d->dividend = v; div_calc(c, true); return true;
    case 0x06c: d->divisor = v; div_calc(c, true); return true;
    case 0x070: d->quotient = v; d->dirty = true; return true;
    case 0x074: d->remainder = v; d->dirty = true; return true;
    default: return off >= 0x180;
    }
}

// Interpolators and TMDS encoders each belong to one bank, as PERI_NONSEC assigns them.
static bool interp_in_bank(chip_t *m, unsigned i, int b, bool *ok)
{
    if ((int)((m->peri_nonsec >> i) & 1) == b)
        return true;
    *ok = false;
    return false;
}

// One bank's view of the SIO, made once for each bank so the Secure one stays as quick as it was.
static inline __attribute__((always_inline)) uint32_t sio_read_bank(cpu_t *c, uint32_t off, int b, bool *ok)
{
    chip_t *m = c->m;
    sio_bank_t *k = &m->sio[b];
    uint32_t rv;
    if (m->rp2040 && rp2040_read(c, off, &rv))
        return rv;
    // The GPIO registers are shared, and the GPIOs Secure code keeps to itself read as zero.
    switch (off) {
    case 0x000: return (uint32_t)c->id;
    case 0x004: return (uint32_t)gpio_in_all(m) & gpio_view(m, b, 0);
    case 0x008: return (uint32_t)(gpio_in_all(m) >> 32) & gpio_view(m, b, 1);
    case 0x010: return m->sio_out[0] & gpio_view(m, b, 0);
    case 0x014: return m->sio_out[1] & gpio_view(m, b, 1);
    case 0x030: return m->sio_oe[0] & gpio_view(m, b, 0);
    case 0x034: return m->sio_oe[1] & gpio_view(m, b, 1);
    case 0x050: {
        sio_fifo_t *rx = &k->fifo[1 - c->id], *tx = &k->fifo[c->id];
        return (rx->count ? 1u : 0) | (tx->count < 8 ? 2u : 0) | (tx->wof ? 4u : 0) | (rx->roe ? 8u : 0);
    }
    case 0x058: {
        sio_fifo_t *rx = &k->fifo[1 - c->id];
        if (!rx->count) {
            rx->roe = true;
            sio_fifo_irq(m);
            return 0;
        }
        uint32_t v = rx->data[rx->head];
        rx->head = (rx->head + 1) & 7;
        rx->count--;
        sio_fifo_irq(m);
        return v;
    }
    case 0x05c: return k->spinlocks;
    case 0x180:
    case 0x184: return k->doorbell[1 - c->id];
    case 0x188:
    case 0x18c: return k->doorbell[c->id];
    // PERI_NONSEC and the RISC-V platform timer are only in the Secure bank.
    case 0x190: return b ? 0 : m->peri_nonsec;
    case 0x1a4: return 0;
    case 0x1b0: return b ? 0 : (uint32_t)mtime(m);
    case 0x1b4: return b ? 0 : (uint32_t)(mtime(m) >> 32);
    default:
        if (off >= 0x100 && off < 0x180) {
            unsigned n = (off - 0x100) >> 2;
            if (k->spinlocks & (1u << n))
                return 0;
            k->spinlocks |= 1u << n;
            return 1u << n;
        }
        if (off >= 0x080 && off < 0x100 && interp_in_bank(m, (off - 0x80) >> 6, b, ok))
            return interp_access(&m->interp[c->id][(off - 0x80) >> 6], (off - 0x80) & 0x3f, false, 0);
        return 0;
    }
}

// Non-secure state always reaches the Non-secure bank, which Secure state reaches through
// SIO_NONSEC_BASE, a mirror that is a bus error to Non-secure state.
uint32_t sio_read(cpu_t *c, uint32_t off, bool *ok)
{
    if (likely(!c->ns && !(off & 0x20000)) || c->m->rp2040)
        return sio_read_bank(c, off & 0xfff, 0, ok);
    if (c->ns && (off & 0x20000)) {
        *ok = false;
        return 0;
    }
    return sio_read_bank(c, off & 0xfff, 1, ok);
}

// Only pins whose output or enable actually changed need recomputing; SPI frames already in flight
// must finish under the old chip-select levels first.
static void gpio_regs_changed(cpu_t *c, uint32_t old_out[2], uint32_t old_oe[2])
{
    chip_t *m = c->m;
    uint64_t diff = ((uint64_t)(m->sio_out[0] ^ old_out[0]) | ((uint64_t)(m->sio_out[1] ^ old_out[1]) << 32)) |
                    ((uint64_t)(m->sio_oe[0] ^ old_oe[0]) | ((uint64_t)(m->sio_oe[1] ^ old_oe[1]) << 32));
    if (!diff)
        return;
    pl022_sync_all(m);
    // The other core may already have run ahead of this one, taking `now` with it.
    m->edge_t = c->t;
    gpio_refresh_mask(m, diff);
    m->edge_t = 0;
}

static inline __attribute__((always_inline)) void sio_write_bank(cpu_t *c, uint32_t off, uint32_t v, int b, bool *ok)
{
    chip_t *m = c->m;
    sio_bank_t *k = &m->sio[b];
    if (m->rp2040 && rp2040_write(c, off, v))
        return;
    if (off >= 0x010 && off < 0x050) {
        uint32_t old_out[2] = {m->sio_out[0], m->sio_out[1]}, old_oe[2] = {m->sio_oe[0], m->sio_oe[1]};
        unsigned reg = (off - 0x10) >> 2;
        bool hi = reg & 1;
        uint32_t *target = (reg >= 8) ? &m->sio_oe[hi] : &m->sio_out[hi];
        uint32_t nv = *target, mask = gpio_view(m, b, hi);
        switch ((reg >> 1) & 3) {
        case 0: nv = v; break;
        case 1: nv |= v; break;
        case 2: nv &= ~v; break;
        case 3: nv ^= v; break;
        }
        *target = (*target & ~mask) | (nv & mask);
        gpio_regs_changed(c, old_out, old_oe);
        return;
    }
    switch (off) {
    case 0x050: {
        sio_fifo_t *rx = &k->fifo[1 - c->id], *tx = &k->fifo[c->id];
        if (v & 4)
            tx->wof = false;
        if (v & 8)
            rx->roe = false;
        sio_fifo_irq(m);
        return;
    }
    case 0x054: {
        sio_fifo_t *tx = &k->fifo[c->id];
        if (tx->count == 8) {
            tx->wof = true;
        } else {
            tx->data[(tx->head + tx->count) & 7] = v;
            tx->count++;
        }
        sio_fifo_irq(m);
        return;
    }
    case 0x180:
        k->doorbell[1 - c->id] |= v & 0xff;
        bell_irq(m);
        return;
    case 0x184:
        k->doorbell[1 - c->id] &= ~v;
        bell_irq(m);
        return;
    case 0x188:
        k->doorbell[c->id] |= v & 0xff;
        bell_irq(m);
        return;
    case 0x18c:
        k->doorbell[c->id] &= ~v;
        bell_irq(m);
        return;
    case 0x190:
        if (!b)
            m->peri_nonsec = v;
        return;
    default:
        if (off >= 0x100 && off < 0x180) {
            k->spinlocks &= ~(1u << ((off - 0x100) >> 2));
            return;
        }
        if (off >= 0x080 && off < 0x100 && interp_in_bank(m, (off - 0x80) >> 6, b, ok))
            interp_access(&m->interp[c->id][(off - 0x80) >> 6], (off - 0x80) & 0x3f, true, v);
        return;
    }
}

void sio_write(cpu_t *c, uint32_t off, uint32_t v, bool *ok)
{
    if (likely(!c->ns && !(off & 0x20000)) || c->m->rp2040) {
        sio_write_bank(c, off & 0xfff, v, 0, ok);
        return;
    }
    if (c->ns && (off & 0x20000)) {
        *ok = false;
        return;
    }
    sio_write_bank(c, off & 0xfff, v, 1, ok);
}

// The GPIO coprocessor reaches the GPIOs as the SIO bank of the core's state does.
static uint32_t gpioc_mask(cpu_t *c, unsigned idx)
{
    return c->ns ? gpio_nsmask(c->m, idx & 1) : ~0u;
}

static void gpioc_apply(cpu_t *c, bool oe, unsigned idx, unsigned op, uint32_t v)
{
    chip_t *m = c->m;
    uint32_t *t = oe ? &m->sio_oe[idx & 1] : &m->sio_out[idx & 1];
    uint32_t nv = *t;
    switch (op) {
    case 0: nv = v; break;
    case 1: nv ^= v; break;
    case 2: nv |= v; break;
    default: nv &= ~v; break;
    }
    *t = (*t & ~gpioc_mask(c, idx)) | (nv & gpioc_mask(c, idx));
}

bool gpioc_mcr(cpu_t *c, unsigned opc1, unsigned crm, unsigned opc2, uint32_t v)
{
    chip_t *m = c->m;
    uint32_t old_out[2] = {m->sio_out[0], m->sio_out[1]}, old_oe[2] = {m->sio_oe[0], m->sio_oe[1]};
    (void)opc2;
    bool oe = crm & 4;
    if (opc1 <= 3) {
        gpioc_apply(c, oe, crm & 1, opc1, v);
    } else if (opc1 >= 5 && opc1 <= 7) {
        unsigned pin = v & 63;
        static const unsigned ops[3] = {1, 2, 3};
        gpioc_apply(c, oe, pin >> 5, ops[opc1 - 5], 1u << (pin & 31));
    } else {
        return false;
    }
    gpio_regs_changed(c, old_out, old_oe);
    return true;
}

bool gpioc_mcrr(cpu_t *c, unsigned opc1, unsigned crm, uint32_t lo, uint32_t hi)
{
    chip_t *m = c->m;
    uint32_t old_out[2] = {m->sio_out[0], m->sio_out[1]}, old_oe[2] = {m->sio_oe[0], m->sio_oe[1]};
    bool oe = crm & 4;
    if (opc1 <= 3) {
        gpioc_apply(c, oe, 0, opc1, lo);
        gpioc_apply(c, oe, 1, opc1, hi);
    } else if (opc1 == 4) {
        unsigned pin = lo & 63;
        uint32_t bit = 1u << (pin & 31);
        gpioc_apply(c, oe, pin >> 5, hi ? 2 : 3, bit);
    } else if (opc1 >= 5 && opc1 <= 7) {
        if (hi & 1) {
            unsigned pin = lo & 63;
            static const unsigned ops[3] = {1, 2, 3};
            gpioc_apply(c, oe, pin >> 5, ops[opc1 - 5], 1u << (pin & 31));
        }
    } else if (opc1 >= 8 && opc1 <= 11) {
        static const unsigned ops[4] = {0, 1, 2, 3};
        gpioc_apply(c, oe, hi & 1, ops[opc1 - 8], lo);
    } else {
        return false;
    }
    gpio_regs_changed(c, old_out, old_oe);
    return true;
}

bool gpioc_mrc(cpu_t *c, unsigned opc1, unsigned crm, unsigned opc2, uint32_t *v)
{
    chip_t *m = c->m;
    (void)opc1;
    (void)opc2;
    switch (crm) {
    case 0: *v = m->sio_out[0] & gpioc_mask(c, 0); return true;
    case 1: *v = m->sio_out[1] & gpioc_mask(c, 1); return true;
    case 4: *v = m->sio_oe[0] & gpioc_mask(c, 0); return true;
    case 5: *v = m->sio_oe[1] & gpioc_mask(c, 1); return true;
    case 8: *v = (uint32_t)gpio_in_all(m) & gpioc_mask(c, 0); return true;
    case 9: *v = (uint32_t)(gpio_in_all(m) >> 32) & gpioc_mask(c, 1); return true;
    default: return false;
    }
}

bool coproc_mcr(cpu_t *c, unsigned cp, unsigned opc1, unsigned crn, unsigned crm, unsigned opc2, uint32_t v)
{
    (void)crn;
    if (cp == 0)
        return gpioc_mcr(c, opc1, crm, opc2, v);
    return false;
}

bool coproc_mrc(cpu_t *c, unsigned cp, unsigned opc1, unsigned crn, unsigned crm, unsigned opc2, uint32_t *v)
{
    (void)crn;
    if (cp == 0)
        return gpioc_mrc(c, opc1, crm, opc2, v);
    return false;
}

bool coproc_mcrr(cpu_t *c, unsigned cp, unsigned opc1, unsigned crm, uint32_t lo, uint32_t hi)
{
    if (cp == 0)
        return gpioc_mcrr(c, opc1, crm, lo, hi);
    return false;
}

bool coproc_mrrc(cpu_t *c, unsigned cp, unsigned opc1, unsigned crm, uint32_t *lo, uint32_t *hi)
{
    chip_t *m = c->m;
    (void)opc1;
    if (cp == 0) {
        uint64_t v;
        switch (crm) {
        case 0: v = m->sio_out[0] | ((uint64_t)m->sio_out[1] << 32); break;
        case 4: v = m->sio_oe[0] | ((uint64_t)m->sio_oe[1] << 32); break;
        case 8: v = gpio_in_all(m); break;
        default: return false;
        }
        *lo = (uint32_t)v & gpioc_mask(c, 0);
        *hi = (uint32_t)(v >> 32) & gpioc_mask(c, 1);
        return true;
    }
    return false;
}

bool coproc_cdp(cpu_t *c, unsigned cp, unsigned opc1, unsigned crd, unsigned crn, unsigned crm, unsigned opc2)
{
    (void)c;
    (void)cp;
    (void)opc1;
    (void)crd;
    (void)crn;
    (void)crm;
    (void)opc2;
    return false;
}
