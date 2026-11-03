#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chip.h"

// Cortex-M33 (ARMv8-M Mainline with DSP and single-precision FPU, Secure state only) for the
// RP2350, and the ARMv6-M subset when v8 is false.

#define F_N 0x80000000u
#define F_Z 0x40000000u
#define F_C 0x20000000u
#define F_V 0x10000000u
#define F_Q 0x08000000u

#define EXC_NMI 2
#define EXC_HARDFAULT 3
#define EXC_MEMMANAGE 4
#define EXC_BUSFAULT 5
#define EXC_USAGEFAULT 6
#define EXC_SECUREFAULT 7
#define EXC_SVC 11
#define EXC_DEBUGMON 12
#define EXC_PENDSV 14
#define EXC_SYSTICK 15

#define UFSR_UNDEFINSTR (1u << 16)
#define UFSR_INVSTATE (1u << 17)
#define UFSR_INVPC (1u << 18)
#define UFSR_NOCP (1u << 19)
#define UFSR_UNALIGNED (1u << 24)
#define UFSR_DIVBYZERO (1u << 25)
#define BFSR_PRECISERR (1u << 9)
#define BFSR_IBUSERR (1u << 8)
#define BFSR_BFARVALID (1u << 15)

void emu_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("[pcsim] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

// ---------------------------------------------------------------------------------------------
// Memory

static inline uint16_t fetch16(cpu_t *c, uint32_t a)
{
    chip_t *m = c->m;
    uint8_t *p = m->rpage[a >> 16];
    if (likely(p)) {
        // Consecutive fetches from one 8-byte cache line cannot miss.
        if (unlikely((a >> 28) == 1) && (a & ~7u) != c->fetch_line) {
            c->cyc += xip_cost(m, a);
            c->fetch_line = a & ~7u;
        }
        return rd16le(p + (a & 0xffff));
    }
    return bus_fetch16_slow(c, a);
}

static void fault_unaligned(cpu_t *c)
{
    if (!c->fault) {
        c->fault = true;
        c->check_exc = true;
        c->fault_usage = true;
        c->cfsr |= UFSR_UNALIGNED;
    }
}

static inline uint32_t ld32(cpu_t *c, uint32_t a)
{
    chip_t *m = c->m;
    c->cyc++;
    uint8_t *p = m->rpage[a >> 16];
    if (likely(p && !(a & 3))) {
        if (unlikely((a >> 28) == 1))
            c->cyc += xip_cost(m, a);
        return rd32le(p + (a & 0xffff));
    }
    if (a & 3) {
        fault_unaligned(c);
        return 0;
    }
    return bus_read(c, a, 4);
}

static inline uint32_t ld16(cpu_t *c, uint32_t a)
{
    chip_t *m = c->m;
    c->cyc++;
    uint8_t *p = m->rpage[a >> 16];
    if (likely(p && !(a & 1))) {
        if (unlikely((a >> 28) == 1))
            c->cyc += xip_cost(m, a);
        return rd16le(p + (a & 0xffff));
    }
    if (a & 1) {
        fault_unaligned(c);
        return 0;
    }
    return bus_read(c, a, 2);
}

static inline uint32_t ld8(cpu_t *c, uint32_t a)
{
    chip_t *m = c->m;
    c->cyc++;
    uint8_t *p = m->rpage[a >> 16];
    if (likely(p)) {
        if (unlikely((a >> 28) == 1))
            c->cyc += xip_cost(m, a);
        return p[a & 0xffff];
    }
    return bus_read(c, a, 1);
}

static inline void st32(cpu_t *c, uint32_t a, uint32_t v)
{
    chip_t *m = c->m;
    c->cyc++;
    uint8_t *p = m->wpage[a >> 16];
    if (likely(p && !(a & 3))) {
        wr32le(p + (a & 0xffff), v);
        return;
    }
    if (a & 3) {
        fault_unaligned(c);
        return;
    }
    bus_write(c, a, v, 4);
}

static inline void st16(cpu_t *c, uint32_t a, uint32_t v)
{
    chip_t *m = c->m;
    c->cyc++;
    uint8_t *p = m->wpage[a >> 16];
    if (likely(p && !(a & 1))) {
        wr16le(p + (a & 0xffff), (uint16_t)v);
        return;
    }
    if (a & 1) {
        fault_unaligned(c);
        return;
    }
    bus_write(c, a, v & 0xffff, 2);
}

static inline void st8(cpu_t *c, uint32_t a, uint32_t v)
{
    chip_t *m = c->m;
    c->cyc++;
    uint8_t *p = m->wpage[a >> 16];
    if (likely(p)) {
        p[a & 0xffff] = (uint8_t)v;
        return;
    }
    bus_write(c, a, v & 0xff, 1);
}

// ARMv8-M allows unaligned single loads and stores unless CCR.UNALIGN_TRP is set.
static uint32_t ld32u(cpu_t *c, uint32_t a)
{
    if (!(a & 3) || !c->v8 || (c->ccr & 8))
        return ld32(c, a);
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v |= ld8(c, a + i) << (8 * i);
    return v;
}

static uint32_t ld16u(cpu_t *c, uint32_t a)
{
    if (!(a & 1) || !c->v8 || (c->ccr & 8))
        return ld16(c, a);
    return ld8(c, a) | (ld8(c, a + 1) << 8);
}

static void st32u(cpu_t *c, uint32_t a, uint32_t v)
{
    if (!(a & 3) || !c->v8 || (c->ccr & 8)) {
        st32(c, a, v);
        return;
    }
    for (int i = 0; i < 4; i++)
        st8(c, a + i, v >> (8 * i));
}

static void st16u(cpu_t *c, uint32_t a, uint32_t v)
{
    if (!(a & 1) || !c->v8 || (c->ccr & 8)) {
        st16(c, a, v);
        return;
    }
    st8(c, a, v);
    st8(c, a + 1, v >> 8);
}

// ---------------------------------------------------------------------------------------------
// Flags and shifts

static inline void set_nz(cpu_t *c, uint32_t r)
{
    c->apsr = (c->apsr & ~(F_N | F_Z)) | (r & F_N) | (r ? 0 : F_Z);
}

static inline void set_nzc(cpu_t *c, uint32_t r, uint32_t carry)
{
    c->apsr = (c->apsr & ~(F_N | F_Z | F_C)) | (r & F_N) | (r ? 0 : F_Z) | (carry ? F_C : 0);
}

static inline uint32_t add_c(cpu_t *c, uint32_t a, uint32_t b, uint32_t cin, bool setflags)
{
    uint64_t u = (uint64_t)a + b + cin;
    uint32_t r = (uint32_t)u;
    if (setflags)
        c->apsr = (c->apsr & 0x0fffffffu) | (r & F_N) | (r ? 0 : F_Z) | ((u >> 32) ? F_C : 0) |
                  ((((a ^ r) & (b ^ r)) >> 31) ? F_V : 0);
    return r;
}

static inline bool cond_pass(cpu_t *c, unsigned cond)
{
    uint32_t f = c->apsr;
    bool n = f & F_N, z = f & F_Z, cy = f & F_C, v = f & F_V;
    switch (cond) {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return cy;
    case 0x3: return !cy;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return cy && !z;
    case 0x9: return !cy || z;
    case 0xa: return n == v;
    case 0xb: return n != v;
    case 0xc: return !z && n == v;
    case 0xd: return z || n != v;
    default: return true;
    }
}

static uint32_t shift_c(uint32_t v, unsigned type, unsigned amount, uint32_t cin, uint32_t *cout)
{
    *cout = cin;
    switch (type) {
    case 0:
        if (amount == 0)
            return v;
        if (amount < 32) {
            *cout = (v >> (32 - amount)) & 1;
            return v << amount;
        }
        *cout = amount == 32 ? (v & 1) : 0;
        return 0;
    case 1:
        if (amount == 0)
            return v;
        if (amount < 32) {
            *cout = (v >> (amount - 1)) & 1;
            return v >> amount;
        }
        *cout = amount == 32 ? (v >> 31) : 0;
        return 0;
    case 2:
        if (amount == 0)
            return v;
        if (amount < 32) {
            *cout = (v >> (amount - 1)) & 1;
            return (uint32_t)((int32_t)v >> amount);
        }
        *cout = v >> 31;
        return (uint32_t)((int32_t)v >> 31);
    case 3:
        if (amount == 0)
            return v;
        amount &= 31;
        if (amount == 0) {
            *cout = v >> 31;
            return v;
        }
        v = (v >> amount) | (v << (32 - amount));
        *cout = v >> 31;
        return v;
    default: {
        uint32_t r = (v >> 1) | (cin << 31);
        *cout = v & 1;
        return r;
    }
    }
}

// DecodeImmShift: type 3 with amount 0 is RRX, encoded here as type 4.
static uint32_t imm_shift_c(uint32_t v, unsigned type, unsigned imm5, uint32_t cin, uint32_t *cout)
{
    if (type == 1 || type == 2) {
        if (imm5 == 0)
            imm5 = 32;
    } else if (type == 3 && imm5 == 0) {
        type = 4;
    }
    return shift_c(v, type, imm5, cin, cout);
}

static uint32_t thumb_expand_imm_c(uint32_t imm12, uint32_t cin, uint32_t *cout)
{
    uint32_t imm8 = imm12 & 0xff;
    *cout = cin;
    if ((imm12 >> 10) == 0) {
        switch ((imm12 >> 8) & 3) {
        case 0: return imm8;
        case 1: return imm8 | (imm8 << 16);
        case 2: return (imm8 << 8) | (imm8 << 24);
        default: return imm8 * 0x01010101u;
        }
    }
    uint32_t unrot = 0x80 | (imm12 & 0x7f);
    unsigned rot = (imm12 >> 7) & 31;
    uint32_t v = (unrot >> rot) | (unrot << (32 - rot));
    *cout = v >> 31;
    return v;
}

// ---------------------------------------------------------------------------------------------
// Exceptions

static inline bool use_psp(const cpu_t *c)
{
    return c->ipsr == 0 && (c->control & 2);
}

static void save_sp(cpu_t *c)
{
    if (use_psp(c))
        c->psp = c->r[13];
    else
        c->msp = c->r[13];
}

static void load_sp(cpu_t *c)
{
    c->r[13] = use_psp(c) ? c->psp : c->msp;
}

static inline bool exc_active(const cpu_t *c, unsigned n)
{
    return c->active[n >> 5] & (1u << (n & 31));
}

static int exc_priority(const cpu_t *c, unsigned n)
{
    switch (n) {
    case 1: return -3;
    case EXC_NMI: return -2;
    case EXC_HARDFAULT: return -1;
    default:
        if (n >= 16)
            return c->nvic_pri[n - 16];
        return c->shpr[n];
    }
}

static int exec_priority(const cpu_t *c, bool with_masks)
{
    int p = 256;
    for (int w = 0; w < 3; w++) {
        uint32_t a = c->active[w];
        while (a) {
            unsigned n = (unsigned)(w * 32 + __builtin_ctz(a));
            a &= a - 1;
            int q = exc_priority(c, n);
            if (q < p)
                p = q;
        }
    }
    if (with_masks) {
        if (c->basepri && (int)c->basepri < p)
            p = (int)c->basepri;
        if (c->primask && p > 0)
            p = 0;
        if (c->faultmask && p > -1)
            p = -1;
    }
    return p;
}

static void active_irq_mask(const cpu_t *c, uint32_t out[2])
{
    out[0] = (c->active[0] >> 16) | (c->active[1] << 16);
    out[1] = (c->active[1] >> 16) | (c->active[2] << 16);
}

static unsigned best_pending(cpu_t *c, int *prio)
{
    uint32_t act[2];
    active_irq_mask(c, act);
    uint64_t lines = (c->m->irq_lines | c->m->irq_core[c->id]);
    c->nvic_pend[0] |= (uint32_t)lines & ~act[0];
    c->nvic_pend[1] |= (uint32_t)(lines >> 32) & ~act[1];
    if (c->pend_nmi) {
        *prio = -2;
        return EXC_NMI;
    }
    unsigned best = 0;
    int bp = 1000;
    if (c->pend_pendsv && c->shpr[EXC_PENDSV] < bp) {
        bp = c->shpr[EXC_PENDSV];
        best = EXC_PENDSV;
    }
    if (c->pend_systick && c->shpr[EXC_SYSTICK] < bp) {
        bp = c->shpr[EXC_SYSTICK];
        best = EXC_SYSTICK;
    }
    for (int w = 0; w < 2; w++) {
        uint32_t p = c->nvic_pend[w] & c->nvic_en[w];
        while (p) {
            unsigned i = (unsigned)(w * 32 + __builtin_ctz(p));
            p &= p - 1;
            if (c->nvic_pri[i] < bp) {
                bp = c->nvic_pri[i];
                best = 16 + i;
            }
        }
    }
    *prio = bp;
    return best;
}

bool cpu_irq_wakes(cpu_t *c)
{
    int bp;
    unsigned n = best_pending(c, &bp);
    return n && bp < exec_priority(c, false);
}

static void lockup(cpu_t *c, const char *why)
{
    emu_log("core %d locked up: %s at pc=%08x", c->id, why, c->r[15]);
    cpu_dump(c);
    c->state = CPU_LOCKUP;
}

static uint32_t xpsr_value(const cpu_t *c)
{
    uint32_t it = c->itstate;
    return (c->apsr & 0xf80f0000u) | ((it & 3) << 25) | (((it >> 2) & 0x3f) << 10) | 0x01000000u | c->ipsr;
}

static void exception_entry(cpu_t *c, unsigned n, uint32_t ret)
{
    save_sp(c);
    bool fp = c->v8 && (c->control & 4);
    uint32_t sp = use_psp(c) ? c->psp : c->msp;
    uint32_t frame_size = fp ? 0x68 : 0x20;
    uint32_t align = (sp & 4) ? 1 : 0;
    sp = (sp - frame_size) & ~7u;
    uint32_t frame[8] = {c->r[0], c->r[1], c->r[2], c->r[3], c->r[12], c->r[14], ret, xpsr_value(c) | (align << 9)};
    for (int i = 0; i < 8; i++)
        st32(c, sp + 4u * i, frame[i]);
    if (fp) {
        for (int i = 0; i < 16; i++)
            st32(c, sp + 0x20 + 4u * i, c->s[i]);
        st32(c, sp + 0x60, c->fpscr);
        c->control &= ~4u;
    }
    if (use_psp(c))
        c->psp = sp;
    else
        c->msp = sp;
    uint32_t exc_ret;
    if (c->ipsr)
        exc_ret = 0xfffffff1u;
    else
        exc_ret = (c->control & 2) ? 0xfffffffdu : 0xfffffff9u;
    if (fp)
        exc_ret &= ~0x10u;
    c->r[14] = exc_ret;
    c->ipsr = n;
    c->itstate = 0;
    c->active[n >> 5] |= 1u << (n & 31);
    c->r[13] = c->msp;
    c->excl_valid = false;
    c->fault = false;
    uint32_t vec = ld32(c, c->vtor + 4 * n);
    if (c->fault || !(vec & 1)) {
        c->fault = false;
        if (n == EXC_HARDFAULT || n == EXC_NMI) {
            lockup(c, "bad vector");
            return;
        }
        c->hfsr |= 2;
        c->active[n >> 5] &= ~(1u << (n & 31));
        exception_entry(c, EXC_HARDFAULT, ret);
        return;
    }
    c->r[15] = vec & ~1u;
    c->cyc += 12;
    c->check_exc = true;
}

static void exception_return(cpu_t *c, uint32_t exc_ret)
{
    unsigned mode = exc_ret & 0xf;
    if (c->ipsr == 0 || (mode != 1 && mode != 9 && mode != 0xd)) {
        lockup(c, "bad EXC_RETURN");
        return;
    }
    c->active[c->ipsr >> 5] &= ~(1u << (c->ipsr & 31));
    if (c->ipsr != EXC_NMI && c->faultmask)
        c->faultmask = 0;
    c->msp = c->r[13];
    bool to_thread = mode != 1;
    if (to_thread)
        c->control = (c->control & ~2u) | ((mode == 0xd) ? 2u : 0u);
    uint32_t sp = (mode == 0xd) ? c->psp : c->msp;
    uint32_t frame[8];
    for (int i = 0; i < 8; i++)
        frame[i] = ld32(c, sp + 4u * i);
    bool fp = c->v8 && !(exc_ret & 0x10);
    if (fp) {
        for (int i = 0; i < 16; i++)
            c->s[i] = ld32(c, sp + 0x20 + 4u * i);
        c->fpscr = ld32(c, sp + 0x60);
        sp += 0x68;
        c->control |= 4u;
    } else {
        sp += 0x20;
        c->control &= ~4u;
    }
    if (frame[7] & (1u << 9))
        sp |= 4;
    if (mode == 0xd)
        c->psp = sp;
    else
        c->msp = sp;
    c->r[0] = frame[0];
    c->r[1] = frame[1];
    c->r[2] = frame[2];
    c->r[3] = frame[3];
    c->r[12] = frame[4];
    c->r[14] = frame[5];
    c->r[15] = frame[6] & ~1u;
    c->apsr = frame[7] & 0xf80f0000u;
    c->itstate = ((frame[7] >> 25) & 3) | (((frame[7] >> 10) & 0x3f) << 2);
    c->ipsr = to_thread ? 0 : (frame[7] & 0x1ff);
    c->excl_valid = false;
    load_sp(c);
    c->cyc += 12;
    c->check_exc = true;
}

static void take_fault(cpu_t *c, unsigned exc, uint32_t pc, const char *why)
{
    c->fault = false;
    c->fault_usage = false;
    if (c->m->trace)
        emu_log("core %d %s at %08x", c->id, why, pc);
    if (c->v8) {
        uint32_t ena = exc == EXC_USAGEFAULT ? (1u << 18) : exc == EXC_BUSFAULT ? (1u << 17) : 0;
        if (exc == EXC_HARDFAULT || !(c->shcsr & ena) || exc_priority(c, exc) >= exec_priority(c, true)) {
            if (exc != EXC_HARDFAULT)
                c->hfsr |= 1u << 30;
            exc = EXC_HARDFAULT;
        }
    } else {
        exc = EXC_HARDFAULT;
    }
    if (exc == EXC_HARDFAULT && (c->ipsr == EXC_HARDFAULT || c->ipsr == EXC_NMI || c->faultmask)) {
        c->r[15] = pc;
        lockup(c, why);
        return;
    }
    if (exc == EXC_USAGEFAULT)
        c->shcsr |= 1u << 12;
    else if (exc == EXC_BUSFAULT)
        c->shcsr |= 1u << 13;
    exception_entry(c, exc, pc);
}

static void undefined(cpu_t *c, uint32_t pc, uint32_t op)
{
    c->cfsr |= UFSR_UNDEFINSTR;
    if (c->m->trace)
        emu_log("core %d undefined instruction %08x at %08x", c->id, op, pc);
    take_fault(c, EXC_USAGEFAULT, pc, "undefined instruction");
}

static inline void branch_interworking(cpu_t *c, uint32_t v, uint32_t pc_of_insn)
{
    if (c->ipsr && (v >> 24) == 0xff) {
        exception_return(c, v);
        return;
    }
    if (!(v & 1)) {
        c->cfsr |= UFSR_INVSTATE;
        c->r[15] = v & ~1u;
        take_fault(c, EXC_USAGEFAULT, v & ~1u, "branch to ARM state");
        (void)pc_of_insn;
        return;
    }
    c->r[15] = v & ~1u;
}

static void check_exceptions(cpu_t *c)
{
    c->check_exc = false;
    int bp;
    unsigned n = best_pending(c, &bp);
    if (!n || bp >= exec_priority(c, true))
        return;
    if (n == EXC_NMI)
        c->pend_nmi = false;
    else if (n == EXC_PENDSV)
        c->pend_pendsv = false;
    else if (n == EXC_SYSTICK)
        c->pend_systick = false;
    else
        c->nvic_pend[(n - 16) >> 5] &= ~(1u << ((n - 16) & 31));
    if (c->state == CPU_WFI || c->state == CPU_WFE)
        c->state = CPU_RUN;
    exception_entry(c, n, c->r[15]);
}

void cpu_wake(cpu_t *c, bool event)
{
    if (event) {
        if (c->state == CPU_WFE)
            c->state = CPU_RUN;
        else
            c->event = true;
    }
    c->check_exc = true;
}

static void sleep_check(cpu_t *c)
{
    if (c->state == CPU_WFI) {
        if (cpu_irq_wakes(c))
            c->state = CPU_RUN;
    } else if (c->state == CPU_WFE) {
        if (c->event) {
            c->event = false;
            c->state = CPU_RUN;
        } else if (cpu_irq_wakes(c)) {
            c->state = CPU_RUN;
        } else if (c->scr & 0x10) {
            uint32_t act[2];
            active_irq_mask(c, act);
            uint64_t lines = (c->m->irq_lines | c->m->irq_core[c->id]);
            if (c->nvic_pend[0] | c->nvic_pend[1] | ((uint32_t)lines & ~act[0]) | ((uint32_t)(lines >> 32) & ~act[1]))
                c->state = CPU_RUN;
        }
    }
}

void cpu_reset(cpu_t *c)
{
    chip_t *m = c->m;
    int id = c->id;
    bool v8 = c->v8;
    ev_cancel(m, &c->syst_ev);
    uint64_t t = c->t;
    memset(c, 0, offsetof(cpu_t, m));
    c->id = id;
    c->v8 = v8;
    c->t = t;
    c->pri_mask = v8 ? 0xf0 : 0xc0;
    c->ccr = v8 ? 0x201 : 0x204;
    c->syst_ev.ctx = c;
    c->msp = rd32le(m->rom + 0) & ~3u;
    c->r[13] = c->msp;
    c->r[14] = 0xffffffffu;
    c->r[15] = rd32le(m->rom + 4) & ~1u;
    c->state = CPU_RUN;
    c->check_exc = true;
}

void cpu_return_from_hle(cpu_t *c)
{
    branch_interworking(c, c->r[14], c->r[15]);
}

// ---------------------------------------------------------------------------------------------
// SysTick

static void systick_fire(chip_t *m, emu_event_t *ev);

static uint64_t systick_period(cpu_t *c)
{
    // CLKSOURCE=0 selects the core's tick from the TICKS block, nominally 1 MHz.
    return (c->syst_csr & 4) ? c->m->sys_ps : PS_PER_US;
}

static uint64_t cpu_now(cpu_t *c)
{
    return c->t > c->m->now ? c->t : c->m->now;
}

static uint32_t systick_current(cpu_t *c)
{
    if (!(c->syst_csr & 1))
        return c->syst_cvr;
    uint64_t ticks = (cpu_now(c) - c->syst_t) / systick_period(c);
    uint64_t reload = (uint64_t)(c->syst_rvr & 0xffffff) + 1;
    if (ticks <= c->syst_cvr)
        return (uint32_t)(c->syst_cvr - ticks);
    ticks -= c->syst_cvr + 1;
    c->syst_countflag = true;
    return (uint32_t)(c->syst_rvr - (ticks % reload));
}

static void systick_schedule(cpu_t *c)
{
    ev_cancel(c->m, &c->syst_ev);
    if (!(c->syst_csr & 1))
        return;
    uint64_t now = cpu_now(c);
    c->syst_ev.fn = systick_fire;
    c->syst_ev.ctx = c;
    ev_schedule(c->m, &c->syst_ev, now + ((uint64_t)c->syst_cvr + 1) * systick_period(c));
}

static void systick_fire(chip_t *m, emu_event_t *ev)
{
    (void)m;
    cpu_t *c = ev->ctx;
    c->syst_countflag = true;
    if (c->syst_csr & 2) {
        c->pend_systick = true;
        c->check_exc = true;
    }
    c->syst_cvr = c->syst_rvr & 0xffffff;
    c->syst_t = ev->when;
    if ((c->syst_csr & 1) && c->syst_rvr) {
        c->syst_ev.fn = systick_fire;
        ev_schedule(c->m, &c->syst_ev, ev->when + ((uint64_t)c->syst_cvr + 1) * systick_period(c));
    }
}

// ---------------------------------------------------------------------------------------------
// System control space

uint32_t cpu_ppb_read(cpu_t *c, uint32_t off)
{
    switch (off) {
    case 0xe004: return 1;
    case 0xe010: {
        systick_current(c);
        uint32_t v = (c->syst_csr & 7) | (c->syst_countflag ? (1u << 16) : 0);
        c->syst_countflag = false;
        return v;
    }
    case 0xe014: return c->syst_rvr & 0xffffff;
    case 0xe018: return systick_current(c);
    case 0xe01c: return 0x40000000u | 9999;
    case 0xed00: return c->v8 ? 0x411fd210u : 0x410cc601u;
    case 0xed04: {
        int bp;
        unsigned pend = best_pending(c, &bp);
        uint32_t v = c->ipsr | (pend << 12);
        if (c->pend_pendsv)
            v |= 1u << 28;
        if (c->pend_systick)
            v |= 1u << 26;
        if (c->pend_nmi)
            v |= 1u << 31;
        if ((c->nvic_pend[0] & c->nvic_en[0]) | (c->nvic_pend[1] & c->nvic_en[1]))
            v |= 1u << 22;
        return v;
    }
    case 0xed08: return c->vtor;
    case 0xed0c: return 0xfa050000u | (c->aircr & 0x700);
    case 0xed10: return c->scr;
    case 0xed14: return c->ccr;
    case 0xed18: return c->shpr[4] | (c->shpr[5] << 8) | (c->shpr[6] << 16) | (c->shpr[7] << 24);
    case 0xed1c: return (uint32_t)c->shpr[11] << 24;
    case 0xed20: return ((uint32_t)c->shpr[15] << 24) | ((uint32_t)c->shpr[14] << 16) | c->shpr[12];
    case 0xed24: {
        uint32_t v = c->shcsr & 0x000f0000u;
        if (exc_active(c, EXC_MEMMANAGE))
            v |= 1;
        if (exc_active(c, EXC_BUSFAULT))
            v |= 2;
        if (exc_active(c, EXC_USAGEFAULT))
            v |= 8;
        if (exc_active(c, EXC_SVC))
            v |= 1u << 7;
        if (exc_active(c, EXC_PENDSV))
            v |= 1u << 10;
        if (exc_active(c, EXC_SYSTICK))
            v |= 1u << 11;
        return v;
    }
    case 0xed28: return c->cfsr;
    case 0xed2c: return c->hfsr;
    case 0xed34: return c->mmfar;
    case 0xed38: return c->bfar;
    case 0xed88: return c->cpacr;
    case 0xed8c: return c->nsacr;
    case 0xed90: return 8u << 8;
    case 0xef34: return c->fpccr;
    case 0xef38: return c->fpcar;
    case 0xef3c: return c->fpdscr;
    default:
        if (off >= 0xe100 && off < 0xe108)
            return c->nvic_en[(off - 0xe100) >> 2];
        if (off >= 0xe180 && off < 0xe188)
            return c->nvic_en[(off - 0xe180) >> 2];
        if ((off >= 0xe200 && off < 0xe208) || (off >= 0xe280 && off < 0xe288)) {
            int bp;
            best_pending(c, &bp);
            return c->nvic_pend[(off & 0x7f) >> 2];
        }
        if (off >= 0xe300 && off < 0xe308) {
            uint32_t act[2];
            active_irq_mask(c, act);
            return act[(off - 0xe300) >> 2];
        }
        if (off >= 0xe400 && off < 0xe440) {
            unsigned i = off - 0xe400;
            return (uint32_t)c->nvic_pri[i] | ((uint32_t)c->nvic_pri[i + 1] << 8) |
                   ((uint32_t)c->nvic_pri[i + 2] << 16) | ((uint32_t)c->nvic_pri[i + 3] << 24);
        }
        return 0;
    }
}

void cpu_ppb_write(cpu_t *c, uint32_t off, uint32_t v)
{
    chip_t *m = c->m;
    switch (off) {
    case 0xe010:
        c->syst_cvr = systick_current(c);
        c->syst_t = cpu_now(c);
        c->syst_csr = v & 7;
        systick_schedule(c);
        break;
    case 0xe014:
        c->syst_rvr = v & 0xffffff;
        break;
    case 0xe018:
        c->syst_cvr = 0;
        c->syst_countflag = false;
        c->syst_t = cpu_now(c);
        systick_schedule(c);
        break;
    case 0xed04:
        if (v & (1u << 31))
            c->pend_nmi = true;
        if (v & (1u << 28))
            c->pend_pendsv = true;
        if (v & (1u << 27))
            c->pend_pendsv = false;
        if (v & (1u << 26))
            c->pend_systick = true;
        if (v & (1u << 25))
            c->pend_systick = false;
        c->check_exc = true;
        break;
    case 0xed08:
        c->vtor = v & 0xffffff80u;
        break;
    case 0xed0c:
        if ((v >> 16) == 0x05fa) {
            c->aircr = v & 0x700;
            if (v & 4)
                chip_request(m, REQ_RESET, "SYSRESETREQ");
        }
        break;
    case 0xed10:
        c->scr = v & 0x1e;
        break;
    case 0xed14:
        c->ccr = (c->ccr & ~0x31au) | (v & 0x31au);
        break;
    case 0xed18:
        for (int k = 0; k < 4; k++)
            c->shpr[4 + k] = (uint8_t)((v >> (8 * k)) & c->pri_mask);
        c->check_exc = true;
        break;
    case 0xed1c:
        c->shpr[11] = (uint8_t)((v >> 24) & c->pri_mask);
        c->check_exc = true;
        break;
    case 0xed20:
        c->shpr[12] = (uint8_t)(v & c->pri_mask);
        c->shpr[14] = (uint8_t)((v >> 16) & c->pri_mask);
        c->shpr[15] = (uint8_t)((v >> 24) & c->pri_mask);
        c->check_exc = true;
        break;
    case 0xed24:
        c->shcsr = v & 0x000f0000u;
        break;
    case 0xed28:
        c->cfsr &= ~v;
        break;
    case 0xed2c:
        c->hfsr &= ~v;
        break;
    case 0xed88:
        c->cpacr = v;
        break;
    case 0xed8c:
        c->nsacr = v;
        break;
    case 0xef34:
        c->fpccr = v;
        break;
    case 0xef38:
        c->fpcar = v;
        break;
    case 0xef3c:
        c->fpdscr = v;
        break;
    default:
        if (off >= 0xe100 && off < 0xe108) {
            c->nvic_en[(off - 0xe100) >> 2] |= v;
            c->check_exc = true;
        } else if (off >= 0xe180 && off < 0xe188) {
            c->nvic_en[(off - 0xe180) >> 2] &= ~v;
        } else if (off >= 0xe200 && off < 0xe208) {
            c->nvic_pend[(off - 0xe200) >> 2] |= v;
            c->check_exc = true;
        } else if (off >= 0xe280 && off < 0xe288) {
            c->nvic_pend[(off - 0xe280) >> 2] &= ~v;
        } else if (off >= 0xe400 && off < 0xe440) {
            unsigned i = off - 0xe400;
            for (int k = 0; k < 4; k++)
                c->nvic_pri[i + k] = (uint8_t)((v >> (8 * k)) & c->pri_mask);
            c->check_exc = true;
        }
        break;
    }
}

static uint32_t read_special(cpu_t *c, unsigned sysm)
{
    switch (sysm) {
    case 0: return c->apsr & 0xf80f0000u;
    case 1:
    case 3: return (c->apsr & 0xf80f0000u) | c->ipsr;
    case 2: return c->apsr & 0xf80f0000u;
    case 5:
    case 7: return c->ipsr;
    case 6: return 0;
    case 8: return use_psp(c) ? c->msp : c->r[13];
    case 9: return use_psp(c) ? c->r[13] : c->psp;
    case 10: return c->msplim;
    case 11: return c->psplim;
    case 16: return c->primask & 1;
    case 17:
    case 18: return c->basepri;
    case 19: return c->faultmask & 1;
    case 20: return c->control & 0xf;
    default: return 0;
    }
}

static void write_special(cpu_t *c, unsigned sysm, uint32_t v, unsigned mask)
{
    switch (sysm) {
    case 0:
    case 1:
    case 2:
    case 3:
        if (mask & 2)
            c->apsr = (c->apsr & 0x000f0000u) | (v & 0xf8000000u);
        if (mask & 1)
            c->apsr = (c->apsr & 0xf8000000u) | (v & 0x000f0000u);
        break;
    case 8:
        if (use_psp(c))
            c->msp = v & ~3u;
        else
            c->r[13] = c->msp = v & ~3u;
        break;
    case 9:
        if (use_psp(c))
            c->r[13] = c->psp = v & ~3u;
        else
            c->psp = v & ~3u;
        break;
    case 10: c->msplim = v & ~7u; break;
    case 11: c->psplim = v & ~7u; break;
    case 16:
        c->primask = v & 1;
        c->check_exc = true;
        break;
    case 17:
        c->basepri = v & c->pri_mask;
        c->check_exc = true;
        break;
    case 18:
        v &= c->pri_mask;
        if (v && (!c->basepri || v < c->basepri))
            c->basepri = v;
        c->check_exc = true;
        break;
    case 19:
        c->faultmask = v & 1;
        c->check_exc = true;
        break;
    case 20:
        if (c->ipsr == 0) {
            save_sp(c);
            c->control = (c->control & ~3u) | (v & 3);
            load_sp(c);
        } else {
            c->control = (c->control & ~1u) | (v & 1);
        }
        c->control = (c->control & ~4u) | (v & 4);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------------------------
// Instruction execution

#define IN_IT(c) ((c)->itstate != 0)

#include "thumb2.inc"
#include "fpu.inc"

static void exec16(cpu_t *c, uint32_t pc, uint16_t op)
{
    uint32_t *r = c->r;
    bool sf = !IN_IT(c);
    switch (op >> 11) {
    case 0x00: {
        unsigned rd = op & 7, rm = (op >> 3) & 7, imm = (op >> 6) & 31;
        uint32_t v = r[rm];
        if (imm == 0) {
            r[rd] = v;
            if (sf)
                set_nz(c, v);
        } else {
            r[rd] = v << imm;
            if (sf)
                set_nzc(c, r[rd], (v >> (32 - imm)) & 1);
        }
        break;
    }
    case 0x01: {
        unsigned rd = op & 7, rm = (op >> 3) & 7, imm = (op >> 6) & 31;
        uint32_t v = r[rm], co;
        r[rd] = imm_shift_c(v, 1, imm, 0, &co);
        if (sf)
            set_nzc(c, r[rd], co);
        break;
    }
    case 0x02: {
        unsigned rd = op & 7, rm = (op >> 3) & 7, imm = (op >> 6) & 31;
        uint32_t v = r[rm], co;
        r[rd] = imm_shift_c(v, 2, imm, 0, &co);
        if (sf)
            set_nzc(c, r[rd], co);
        break;
    }
    case 0x03: {
        unsigned rd = op & 7, rn = (op >> 3) & 7, x = (op >> 6) & 7;
        uint32_t b = (op & 0x400) ? x : r[x];
        if (op & 0x200)
            r[rd] = add_c(c, r[rn], ~b, 1, sf);
        else
            r[rd] = add_c(c, r[rn], b, 0, sf);
        break;
    }
    case 0x04: {
        unsigned rd = (op >> 8) & 7;
        r[rd] = op & 0xff;
        if (sf)
            set_nz(c, r[rd]);
        break;
    }
    case 0x05: add_c(c, r[(op >> 8) & 7], ~(uint32_t)(op & 0xff), 1, true); break;
    case 0x06: {
        unsigned rd = (op >> 8) & 7;
        r[rd] = add_c(c, r[rd], op & 0xff, 0, sf);
        break;
    }
    case 0x07: {
        unsigned rd = (op >> 8) & 7;
        r[rd] = add_c(c, r[rd], ~(uint32_t)(op & 0xff), 1, sf);
        break;
    }
    case 0x08: {
        if (!(op & 0x400)) {
            unsigned rdn = op & 7, rm = (op >> 3) & 7;
            uint32_t a = r[rdn], b = r[rm], co;
            uint32_t cin = (c->apsr & F_C) ? 1 : 0;
            switch ((op >> 6) & 0xf) {
            case 0x0: r[rdn] = a & b; if (sf) set_nz(c, r[rdn]); break;
            case 0x1: r[rdn] = a ^ b; if (sf) set_nz(c, r[rdn]); break;
            case 0x2: r[rdn] = shift_c(a, 0, b & 0xff, cin, &co); if (sf) set_nzc(c, r[rdn], co); break;
            case 0x3: r[rdn] = shift_c(a, 1, b & 0xff, cin, &co); if (sf) set_nzc(c, r[rdn], co); break;
            case 0x4: r[rdn] = shift_c(a, 2, b & 0xff, cin, &co); if (sf) set_nzc(c, r[rdn], co); break;
            case 0x5: r[rdn] = add_c(c, a, b, cin, sf); break;
            case 0x6: r[rdn] = add_c(c, a, ~b, cin, sf); break;
            case 0x7: r[rdn] = shift_c(a, 3, b & 0xff, cin, &co); if (sf) set_nzc(c, r[rdn], co); break;
            case 0x8: set_nz(c, a & b); break;
            case 0x9: r[rdn] = add_c(c, 0, ~b, 1, sf); break;
            case 0xa: add_c(c, a, ~b, 1, true); break;
            case 0xb: add_c(c, a, b, 0, true); break;
            case 0xc: r[rdn] = a | b; if (sf) set_nz(c, r[rdn]); break;
            case 0xd: r[rdn] = a * b; if (sf) set_nz(c, r[rdn]); break;
            case 0xe: r[rdn] = a & ~b; if (sf) set_nz(c, r[rdn]); break;
            case 0xf: r[rdn] = ~b; if (sf) set_nz(c, r[rdn]); break;
            }
        } else {
            unsigned rm = (op >> 3) & 0xf;
            unsigned rdn = (op & 7) | ((op >> 4) & 8);
            uint32_t vm = rm == 15 ? pc + 4 : r[rm];
            switch ((op >> 8) & 3) {
            case 0: {
                uint32_t res = (rdn == 15 ? pc + 4 : r[rdn]) + vm;
                if (rdn == 15) {
                    r[15] = res & ~1u;
                    c->cyc++;
                } else {
                    r[rdn] = res;
                }
                break;
            }
            case 1:
                add_c(c, rdn == 15 ? pc + 4 : r[rdn], ~vm, 1, true);
                break;
            case 2:
                if (rdn == 15) {
                    r[15] = vm & ~1u;
                    c->cyc++;
                } else {
                    r[rdn] = vm;
                }
                break;
            case 3:
                c->cyc++;
                if (op & 0x80)
                    r[14] = (pc + 2) | 1;
                branch_interworking(c, vm, pc);
                break;
            }
        }
        break;
    }
    case 0x09:
        r[(op >> 8) & 7] = ld32(c, ((pc + 4) & ~3u) + ((op & 0xff) << 2));
        break;
    case 0x0a:
    case 0x0b: {
        unsigned rt = op & 7, rn = (op >> 3) & 7, rm = (op >> 6) & 7;
        uint32_t a = r[rn] + r[rm];
        switch ((op >> 9) & 7) {
        case 0: st32u(c, a, r[rt]); break;
        case 1: st16u(c, a, r[rt]); break;
        case 2: st8(c, a, r[rt]); break;
        case 3: r[rt] = (uint32_t)(int32_t)(int8_t)ld8(c, a); break;
        case 4: r[rt] = ld32u(c, a); break;
        case 5: r[rt] = ld16u(c, a); break;
        case 6: r[rt] = ld8(c, a); break;
        case 7: r[rt] = (uint32_t)(int32_t)(int16_t)ld16u(c, a); break;
        }
        break;
    }
    case 0x0c: st32u(c, r[(op >> 3) & 7] + (((op >> 6) & 31) << 2), r[op & 7]); break;
    case 0x0d: r[op & 7] = ld32u(c, r[(op >> 3) & 7] + (((op >> 6) & 31) << 2)); break;
    case 0x0e: st8(c, r[(op >> 3) & 7] + ((op >> 6) & 31), r[op & 7]); break;
    case 0x0f: r[op & 7] = ld8(c, r[(op >> 3) & 7] + ((op >> 6) & 31)); break;
    case 0x10: st16u(c, r[(op >> 3) & 7] + (((op >> 6) & 31) << 1), r[op & 7]); break;
    case 0x11: r[op & 7] = ld16u(c, r[(op >> 3) & 7] + (((op >> 6) & 31) << 1)); break;
    case 0x12: st32u(c, r[13] + ((op & 0xff) << 2), r[(op >> 8) & 7]); break;
    case 0x13: r[(op >> 8) & 7] = ld32u(c, r[13] + ((op & 0xff) << 2)); break;
    case 0x14: r[(op >> 8) & 7] = ((pc + 4) & ~3u) + ((op & 0xff) << 2); break;
    case 0x15: r[(op >> 8) & 7] = r[13] + ((op & 0xff) << 2); break;
    case 0x16:
    case 0x17: {
        if (c->v8 && (op & 0xf500) == 0xb100) {
            unsigned rn = op & 7;
            uint32_t imm = (((op >> 9) & 1) << 6) | (((op >> 3) & 31) << 1);
            bool nz = op & 0x800;
            if ((r[rn] != 0) == nz) {
                r[15] = pc + 4 + imm;
                c->cyc++;
            }
            break;
        }
        switch ((op >> 8) & 0xf) {
        case 0x0:
            if (op & 0x80)
                r[13] -= (op & 0x7f) << 2;
            else
                r[13] += (op & 0x7f) << 2;
            break;
        case 0x2: {
            unsigned rd = op & 7;
            uint32_t v = r[(op >> 3) & 7];
            switch ((op >> 6) & 3) {
            case 0: r[rd] = (uint32_t)(int32_t)(int16_t)v; break;
            case 1: r[rd] = (uint32_t)(int32_t)(int8_t)v; break;
            case 2: r[rd] = v & 0xffff; break;
            case 3: r[rd] = v & 0xff; break;
            }
            break;
        }
        case 0x4:
        case 0x5: {
            uint32_t list = (op & 0xff) | ((op & 0x100) ? 0x4000 : 0);
            uint32_t n = (uint32_t)__builtin_popcount(list);
            uint32_t a = r[13] - 4 * n;
            uint32_t sp = a;
            for (int i = 0; i < 15; i++)
                if (list & (1u << i)) {
                    st32(c, a, r[i]);
                    a += 4;
                }
            if (!c->fault)
                r[13] = sp;
            break;
        }
        case 0x6:
            if ((op & 0xffec) == 0xb660) {
                bool disable = op & 0x10;
                if (op & 2)
                    c->primask = disable;
                if (op & 1)
                    c->faultmask = disable;
                c->check_exc = true;
            } else {
                undefined(c, pc, op);
            }
            break;
        case 0xa: {
            unsigned rd = op & 7;
            uint32_t v = r[(op >> 3) & 7];
            switch ((op >> 6) & 3) {
            case 0: r[rd] = __builtin_bswap32(v); break;
            case 1: r[rd] = ((v & 0xff00ff00u) >> 8) | ((v & 0x00ff00ffu) << 8); break;
            case 3: r[rd] = (uint32_t)(int32_t)(int16_t)(((v & 0xff) << 8) | ((v >> 8) & 0xff)); break;
            default: undefined(c, pc, op); break;
            }
            break;
        }
        case 0xc:
        case 0xd: {
            uint32_t list = op & 0xff;
            uint32_t a = r[13];
            uint32_t vals[8];
            for (int i = 0; i < 8; i++)
                if (list & (1u << i)) {
                    vals[i] = ld32(c, a);
                    a += 4;
                }
            uint32_t newpc = 0;
            if (op & 0x100) {
                newpc = ld32(c, a);
                a += 4;
            }
            if (c->fault)
                break;
            for (int i = 0; i < 8; i++)
                if (list & (1u << i))
                    r[i] = vals[i];
            r[13] = a;
            if (op & 0x100) {
                c->cyc += 2;
                branch_interworking(c, newpc, pc);
            }
            break;
        }
        case 0xe:
            if (c->m->trace)
                emu_log("core %d BKPT %u at %08x", c->id, op & 0xff, pc);
            take_fault(c, EXC_HARDFAULT, pc, "breakpoint");
            break;
        case 0xf:
            if (c->v8 && (op & 0xf)) {
                c->itstate = op & 0xff;
                return;
            }
            switch ((op >> 4) & 0xf) {
            case 0x0:
            case 0x1:
                break;
            case 0x2:
                if (c->event)
                    c->event = false;
                else
                    c->state = CPU_WFE;
                break;
            case 0x3:
                c->state = CPU_WFI;
                break;
            case 0x4:
                chip_sev(c->m, c->id);
                break;
            default:
                break;
            }
            break;
        default:
            undefined(c, pc, op);
            break;
        }
        break;
    }
    case 0x18: {
        unsigned rn = (op >> 8) & 7;
        uint32_t a = r[rn];
        for (int i = 0; i < 8; i++)
            if (op & (1u << i)) {
                st32(c, a, r[i]);
                a += 4;
            }
        r[rn] = a;
        break;
    }
    case 0x19: {
        unsigned rn = (op >> 8) & 7;
        uint32_t a = r[rn];
        for (int i = 0; i < 8; i++)
            if (op & (1u << i)) {
                r[i] = ld32(c, a);
                a += 4;
            }
        if (!(op & (1u << rn)))
            r[rn] = a;
        break;
    }
    case 0x1a:
    case 0x1b: {
        unsigned cond = (op >> 8) & 0xf;
        if (cond == 0xe) {
            if (pc < ROM_SIZE) {
                rom_hle(c, op & 0xff);
            } else {
                undefined(c, pc, op);
            }
        } else if (cond == 0xf) {
            if (exc_priority(c, EXC_SVC) >= exec_priority(c, true))
                take_fault(c, EXC_HARDFAULT, pc, "SVC at too high a priority");
            else
                exception_entry(c, EXC_SVC, pc + 2);
        } else if (cond_pass(c, cond)) {
            r[15] = pc + 4 + ((uint32_t)(int32_t)(int8_t)(op & 0xff) << 1);
            c->cyc++;
        }
        break;
    }
    case 0x1c: {
        uint32_t imm = (uint32_t)(op & 0x7ff) << 1;
        if (imm & 0x800)
            imm |= 0xfffff000u;
        r[15] = pc + 4 + imm;
        c->cyc++;
        break;
    }
    default:
        undefined(c, pc, op);
        break;
    }
}

static inline void it_advance(cpu_t *c)
{
    if ((c->itstate & 7) == 0)
        c->itstate = 0;
    else
        c->itstate = (c->itstate & 0xe0) | ((c->itstate << 1) & 0x1f);
}

static void step(cpu_t *c)
{
    uint32_t pc = c->r[15];
    uint16_t op = fetch16(c, pc);
    bool wide = (op >> 11) >= 0x1d;
    c->cyc++;

    uint32_t it = c->itstate;
    if (unlikely(it)) {
        if (!cond_pass(c, it >> 4)) {
            c->r[15] = pc + (wide ? 4 : 2);
            it_advance(c);
            return;
        }
    }
    if (wide) {
        uint16_t hw2 = fetch16(c, pc + 2);
        c->r[15] = pc + 4;
        if (!c->v8 && !((op & 0xf800) == 0xf000 && (hw2 & 0xd000) == 0xd000) && !((op & 0xffe0) == 0xf3e0) &&
            !((op & 0xfff0) == 0xf380) && op != 0xf3bf && op != 0xf3af)
            undefined(c, pc, ((uint32_t)op << 16) | hw2);
        else
            exec32(c, pc, op, hw2);
    } else {
        c->r[15] = pc + 2;
        exec16(c, pc, op);
    }
    if (unlikely(c->fault)) {
        unsigned exc = c->fault_usage ? EXC_USAGEFAULT : EXC_BUSFAULT;
        c->r[15] = pc;
        c->itstate = it;
        take_fault(c, exc, pc, exc == EXC_USAGEFAULT ? "unaligned access" : "bus fault");
        return;
    }
    if (it && c->itstate == it)
        it_advance(c);
}

#include "block.inc"

void cpu_run(cpu_t *c, uint64_t until)
{
    chip_t *m = c->m;
    bool use_blocks = c->v8 && blocks_on();
    if (c->state == CPU_WFI || c->state == CPU_WFE)
        sleep_check(c);
    while (c->t < until) {
        if (unlikely(c->check_exc)) {
            check_exceptions(c);
            if (c->state != CPU_RUN)
                sleep_check(c);
        }
        if (unlikely(c->state != CPU_RUN))
            return;
        if (likely(use_blocks && !c->itstate)) {
            const block_t *b = block_find(m, c->r[15]);
            if (likely(b)) {
                block_run(c, b, until);
                continue;
            }
        }
        c->cyc = 0;
        step(c);
        c->insns++;
        c->t += (uint64_t)c->cyc * m->sys_ps;
    }
}

const char *cpu_state_name(const cpu_t *c)
{
    switch (c->state) {
    case CPU_RUN: return "running";
    case CPU_WFI: return "WFI";
    case CPU_WFE: return "WFE";
    case CPU_OFF: return "held in reset";
    default: return "locked up";
    }
}

void cpu_dump(const cpu_t *c)
{
    emu_log("core %d %s pc=%08x lr=%08x sp=%08x psr=%08x ipsr=%u primask=%u", c->id, cpu_state_name(c), c->r[15],
            c->r[14], c->r[13], c->apsr, c->ipsr, c->primask);
    emu_log("  r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x", c->r[0], c->r[1], c->r[2], c->r[3],
            c->r[4], c->r[5], c->r[6], c->r[7]);
    emu_log("  r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x cfsr=%08x hfsr=%08x bfar=%08x", c->r[8], c->r[9], c->r[10],
            c->r[11], c->r[12], c->cfsr, c->hfsr, c->bfar);
}
