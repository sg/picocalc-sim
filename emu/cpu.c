#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chip.h"

// Cortex-M33 (ARMv8-M Mainline with DSP, single-precision FPU and the security extension) for
// the RP2350, and the ARMv6-M subset when v8 is false.

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
#define SFSR_INVEP (1u << 0)
#define SFSR_INVIS (1u << 1)
#define SFSR_INVER (1u << 2)
#define SFSR_AUVIOL (1u << 3)
#define SFSR_INVTRAN (1u << 4)
#define SFSR_SFARVALID (1u << 6)

#define EXC_RETURN_ES 0x01u
#define EXC_RETURN_SPSEL 0x04u
#define EXC_RETURN_MODE 0x08u
#define EXC_RETURN_FTYPE 0x10u
#define EXC_RETURN_DCRS 0x20u
#define EXC_RETURN_S 0x40u

#define AIRCR_SYSRESETREQS (1u << 3)
#define AIRCR_BFHFNMINS (1u << 13)
#define AIRCR_PRIS (1u << 14)
#define CONTROL_SFPA 8u
#define FPCCR_TS (1u << 26)
#define FNC_RETURN 0xfeffffffu

void emu_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("[picocalc-sim] ", stderr);
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

// UsageFault and MemManage syndromes are banked; BusFault ones are not.
static inline void ufsr_set(cpu_t *c, uint32_t bits)
{
    *(c->ns ? &c->cfsr_ns : &c->cfsr) |= bits;
}

static inline uint32_t cur_ccr(const cpu_t *c)
{
    return c->ns ? c->ccr_ns : c->ccr;
}

static void fault_unaligned(cpu_t *c)
{
    if (!c->fault) {
        c->fault = true;
        c->check_exc = true;
        c->fault_exc = EXC_USAGEFAULT;
        ufsr_set(c, UFSR_UNALIGNED);
    }
}

static inline uint32_t ld32(cpu_t *c, uint32_t a)
{
    chip_t *m = c->m;
    c->cyc++;
    uint8_t *p = c->rpage[a >> 16];
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
    uint8_t *p = c->rpage[a >> 16];
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
    uint8_t *p = c->rpage[a >> 16];
    if (likely(p)) {
        if (unlikely((a >> 28) == 1))
            c->cyc += xip_cost(m, a);
        return p[a & 0xffff];
    }
    return bus_read(c, a, 1);
}

static inline void st32(cpu_t *c, uint32_t a, uint32_t v)
{
    c->cyc++;
    uint8_t *p = c->wpage[a >> 16];
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
    c->cyc++;
    uint8_t *p = c->wpage[a >> 16];
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
    c->cyc++;
    uint8_t *p = c->wpage[a >> 16];
    if (likely(p)) {
        p[a & 0xffff] = (uint8_t)v;
        return;
    }
    bus_write(c, a, v & 0xff, 1);
}

// ARMv8-M allows unaligned single loads and stores unless CCR.UNALIGN_TRP is set.
static uint32_t ld32u(cpu_t *c, uint32_t a)
{
    if (!(a & 3) || !c->v8 || (cur_ccr(c) & 8))
        return ld32(c, a);
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v |= ld8(c, a + i) << (8 * i);
    return v;
}

static uint32_t ld16u(cpu_t *c, uint32_t a)
{
    if (!(a & 1) || !c->v8 || (cur_ccr(c) & 8))
        return ld16(c, a);
    return ld8(c, a) | (ld8(c, a + 1) << 8);
}

static void st32u(cpu_t *c, uint32_t a, uint32_t v)
{
    if (!(a & 3) || !c->v8 || (cur_ccr(c) & 8)) {
        st32(c, a, v);
        return;
    }
    for (int i = 0; i < 4; i++)
        st8(c, a + i, v >> (8 * i));
}

static void st16u(cpu_t *c, uint32_t a, uint32_t v)
{
    if (!(a & 1) || !c->v8 || (cur_ccr(c) & 8)) {
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
// Security attribution

typedef struct {
    bool exempt, ns, nsc, valid;
    uint8_t region;
    uint32_t lo, hi;
} idau_t;

// The RP2350's IDAU (datasheet section 10.2.2) and the range around a it answers alike for, which
// numbers only the Secure gateway region, as region 2, as the A4 boot ROM's TT checks show.
static idau_t idau_check(uint32_t a, bool ifetch)
{
    idau_t d = {.ns = true, .nsc = true};
    if (a < 0x10000000u) {
        // Unmapped addresses below XIP repeat the ROM's map every 32 KB.
        uint32_t base = a & ~0x7fffu, o = a & 0x7fffu;
        if (o < 0x4300) {
            d.exempt = true;
            d.lo = base;
            d.hi = base + 0x42ff;
        } else if (o < 0x7e00) {
            // NSBOOT and the RISC-V emulator are Non-secure only to the instruction side.
            d.exempt = !ifetch;
            d.lo = base + 0x4300;
            d.hi = base + 0x7dff;
        } else {
            d.ns = false;
            d.region = 2;
            d.valid = true;
            d.lo = base + 0x7e00;
            d.hi = base + 0x7fff;
        }
        return d;
    }
    switch (a >> 28) {
    case 0x4:
    case 0x5:
    case 0xd:
        d.exempt = true;
        break;
    default:
        break;
    }
    d.lo = a & 0xf0000000u;
    d.hi = a | 0x0fffffffu;
    return d;
}

typedef struct {
    bool ns, nsc, srvalid, irvalid;
    uint8_t sregion, iregion;
} sattr_t;

// The architecture's system ranges that the SAU and IDAU never attribute.
static bool system_exempt(uint32_t a)
{
    return a <= 0xe0003fffu || (a >= 0xe0005000u && a <= 0xe0005fffu) || (a >= 0xe000e000u && a <= 0xe000efffu) ||
           (a >= 0xe002e000u && a <= 0xe002efffu) || (a >= 0xe0040000u && a <= 0xe0041fffu) ||
           (a >= 0xe00ff000u && a <= 0xe00fffffu);
}

// SecurityCheck() from the Armv8-M Architecture Reference Manual, also narrowing [*lo, *hi] to
// addresses around a that the SAU and IDAU attribute the same way.
static sattr_t security_check(const cpu_t *c, uint32_t a, bool ifetch, bool secure, uint32_t *lo, uint32_t *hi)
{
    sattr_t r = {0};
    idau_t d = idau_check(a, ifetch);
    *lo = d.lo;
    *hi = d.hi;
    if (ifetch && a >= 0xf0000000u)
        return r;
    if (d.exempt || (ifetch && (a >> 28) == 0xe) || ((a >> 28) == 0xe && system_exempt(a))) {
        if ((a >> 28) == 0xe && !ifetch)
            *lo = *hi = a;
        r.ns = !secure;
        return r;
    }
    r.iregion = d.region;
    r.irvalid = d.valid;
    if (c->sau_ctrl & 1) {
        bool multi = false;
        for (unsigned i = 0; i < 8; i++) {
            uint32_t rlar = c->sau_rlar[i];
            if (!(rlar & 1))
                continue;
            uint32_t b = c->sau_rbar[i] & ~0x1fu, l = rlar | 0x1fu;
            if (a >= b && a <= l) {
                if (b > *lo)
                    *lo = b;
                if (l < *hi)
                    *hi = l;
                if (r.srvalid) {
                    multi = true;
                } else {
                    r.ns = !(rlar & 2);
                    r.nsc = (rlar & 2) != 0;
                    r.srvalid = true;
                    r.sregion = (uint8_t)i;
                }
            } else if (b > a) {
                if (b - 1 < *hi)
                    *hi = b - 1;
            } else if (l + 1 > *lo) {
                *lo = l + 1;
            }
        }
        if (multi) {
            r.ns = r.nsc = r.srvalid = false;
            r.sregion = 0;
        }
    } else if (c->sau_ctrl & 2) {
        r.ns = true;
    }
    if (!d.ns && (r.ns || (!d.nsc && r.nsc))) {
        r.ns = false;
        r.nsc = d.nsc;
    }
    return r;
}

static void flag_secure_fault(cpu_t *c, uint32_t sfsr)
{
    if (!c->fault) {
        c->fault = true;
        c->check_exc = true;
        c->fault_exc = EXC_SECUREFAULT;
        c->sfsr |= sfsr;
    }
}

bool cpu_attr_ns(cpu_t *c, uint32_t a)
{
    uint32_t lo, hi;
    return security_check(c, a, false, !c->ns, &lo, &hi).ns;
}

bool cpu_ns_access(cpu_t *c, uint32_t a)
{
    if (a - c->dw_lo < c->dw_size)
        return true;
    uint32_t lo, hi;
    if (security_check(c, a, false, false, &lo, &hi).ns) {
        c->dw_lo = lo;
        c->dw_size = hi - lo;
        return true;
    }
    if (c->m->trace)
        emu_log("core %d Non-secure access to Secure address %08x (pc %08x)", c->id, a, c->r[15]);
    if (!c->fault)
        c->sfar = a;
    flag_secure_fault(c, SFSR_AUVIOL | SFSR_SFARVALID);
    return false;
}

// ---------------------------------------------------------------------------------------------
// Exceptions

static int exec_priority(const cpu_t *c, bool with_masks);

static inline uint32_t cur_control(const cpu_t *c)
{
    return c->ns ? c->control_ns : c->control;
}

static inline uint32_t *control_reg(cpu_t *c, bool secure)
{
    return secure ? &c->control : &c->control_ns;
}

static inline bool use_psp(const cpu_t *c)
{
    return c->ipsr == 0 && (cur_control(c) & 2);
}

static inline bool privileged(const cpu_t *c, bool secure)
{
    return c->ipsr != 0 || !((secure ? c->control : c->control_ns) & 1);
}

static uint32_t *sp_slot(cpu_t *c, bool secure, bool process)
{
    if (secure)
        return process ? &c->psp : &c->msp;
    return process ? &c->psp_ns : &c->msp_ns;
}

static void save_sp(cpu_t *c)
{
    *sp_slot(c, !c->ns, use_psp(c)) = c->r[13];
}

static void load_sp(cpu_t *c)
{
    c->r[13] = *sp_slot(c, !c->ns, use_psp(c));
}

static uint32_t sp_get(cpu_t *c, bool secure, bool process)
{
    if (secure == !c->ns && process == use_psp(c))
        return c->r[13];
    return *sp_slot(c, secure, process);
}

static void sp_set(cpu_t *c, bool secure, bool process, uint32_t v)
{
    if (secure == !c->ns && process == use_psp(c))
        c->r[13] = v;
    else
        *sp_slot(c, secure, process) = v;
}

static uint8_t *no_pages[0x10000];

// Changes the security state with r[13] already saved, and makes the predecoded blocks and native
// code, which run only Secure code, hand over to stepping.
static void switch_state(cpu_t *c, bool ns)
{
    c->ns = ns;
    c->bus_ns = ns || (c->id == 1 && (c->m->accessctrl[1] & 2));
    c->rpage = ns ? no_pages : c->m->rpage;
    c->wpage = ns ? no_pages : c->m->wpage;
    c->fw_size = 0;
    c->check_exc = true;
}

static void set_state(cpu_t *c, bool ns)
{
    save_sp(c);
    switch_state(c, ns);
    load_sp(c);
}

static inline bool exc_active(const cpu_t *c, unsigned n)
{
    return c->active[n >> 5] & (1u << (n & 31));
}

static inline bool bfhfnmins(const cpu_t *c)
{
    return c->aircr & AIRCR_BFHFNMINS;
}

// Exceptions with an instance for each security state; the others go to one state at a time.
static inline bool exc_banked(unsigned n)
{
    return n < 16 && ((0xc858u >> n) & 1);
}

// ExceptionTargetsSecure(): for a banked exception, secure is the state that raised it.
static bool exc_targets_secure(const cpu_t *c, unsigned n, bool secure)
{
    if (!c->v8)
        return true;
    switch (n) {
    case EXC_NMI:
    case EXC_BUSFAULT: return !bfhfnmins(c);
    case EXC_HARDFAULT: return secure || !bfhfnmins(c);
    case EXC_SECUREFAULT: return true;
    default:
        if (n >= 16)
            return !(c->nvic_itns[(n - 16) >> 5] & (1u << ((n - 16) & 31)));
        return secure;
    }
}

static bool exc_active_for(const cpu_t *c, unsigned n, bool secure)
{
    if (exc_banked(n) && !secure)
        return c->active_ns & (1u << n);
    return exc_active(c, n) && exc_targets_secure(c, n, secure) == secure;
}

static void set_active(cpu_t *c, unsigned n, bool secure, bool on)
{
    uint32_t *w = exc_banked(n) && !secure ? &c->active_ns : &c->active[n >> 5];
    uint32_t bit = 1u << (n & 31);
    *w = on ? *w | bit : *w & ~bit;
}

static int group_priority(const cpu_t *c, int p, bool secure)
{
    unsigned prigroup = ((secure ? c->aircr : c->aircr_ns) >> 8) & 7;
    return p & ~((2 << prigroup) - 1);
}

// ExceptionPriority(): group selects the group priority, which decides preemption, rather than
// the whole priority, which orders pending exceptions.
static int exc_priority(const cpu_t *c, unsigned n, bool secure, bool group)
{
    int p;
    switch (n) {
    case 1: return -4;
    case EXC_NMI: return -2;
    case EXC_HARDFAULT: return secure && bfhfnmins(c) ? -3 : -1;
    case EXC_BUSFAULT:
    case EXC_SECUREFAULT:
    case EXC_DEBUGMON: p = c->shpr[n]; break;
    default:
        if (n >= 16)
            p = c->nvic_pri[n - 16];
        else if (exc_banked(n))
            p = secure ? c->shpr[n] : c->shpr_ns[n];
        else
            p = 256;
        break;
    }
    if (p < 256) {
        if (group)
            p = group_priority(c, p, secure);
        if ((c->aircr & AIRCR_PRIS) && !secure)
            p = (p >> 1) + 0x80;
    }
    return p;
}

static int raw_exec_priority(const cpu_t *c)
{
    int p = 256;
    for (int w = 0; w < 3; w++) {
        uint32_t a = c->active[w];
        while (a) {
            unsigned n = (unsigned)(w * 32 + __builtin_ctz(a));
            a &= a - 1;
            int q = exc_priority(c, n, exc_targets_secure(c, n, true), true);
            if (q < p)
                p = q;
        }
    }
    for (uint32_t a = c->active_ns; a; a &= a - 1) {
        int q = exc_priority(c, (unsigned)__builtin_ctz(a), false, true);
        if (q < p)
            p = q;
    }
    return p;
}

// ExecutionPriority(): the masks of both states boost it, the Non-secure ones only to 0x80 when
// AIRCR.PRIS deprioritizes Non-secure exceptions.
static int exec_priority(const cpu_t *c, bool with_masks)
{
    int p = raw_exec_priority(c);
    if (!with_masks)
        return p;
    bool pris = c->aircr & AIRCR_PRIS;
    int boost = 256;
    if (c->basepri_ns) {
        int b = group_priority(c, (int)c->basepri_ns, false);
        boost = pris ? (b >> 1) + 0x80 : b;
    }
    if (c->basepri) {
        int b = group_priority(c, (int)c->basepri, true);
        if (b < boost)
            boost = b;
    }
    if (c->primask_ns & 1)
        boost = !pris ? 0 : boost > 0x80 ? 0x80 : boost;
    if (c->primask & 1)
        boost = 0;
    if (c->faultmask_ns & 1) {
        if (bfhfnmins(c))
            boost = -1;
        else
            boost = !pris ? 0 : boost > 0x80 ? 0x80 : boost;
    }
    if (c->faultmask & 1)
        boost = bfhfnmins(c) ? -3 : -1;
    return boost < p ? boost : p;
}

static void active_irq_mask(const cpu_t *c, uint32_t out[2])
{
    out[0] = (c->active[0] >> 16) | (c->active[1] << 16);
    out[1] = (c->active[1] >> 16) | (c->active[2] << 16);
}

// best_pending once the security extension is in use: banked exceptions pending, interrupts going
// to Non-secure state, or a PRIGROUP that reaches the implemented priority bits.
static __attribute__((noinline)) unsigned best_pending_full(cpu_t *c, int *prio, bool *secure)
{
    unsigned best = 0;
    int bp = 1000;
    bool bs = true;
    // PendSV before SysTick and Secure before Non-secure at equal priority.
    const bool pending[4] = {c->pend_pendsv, c->pend_pendsv_ns, c->pend_systick, c->pend_systick_ns};
    for (unsigned k = 0; k < 4; k++) {
        if (!pending[k])
            continue;
        unsigned n = k < 2 ? EXC_PENDSV : EXC_SYSTICK;
        bool s = !(k & 1);
        int q = exc_priority(c, n, s, false);
        if (q < bp) {
            bp = q;
            best = n;
            bs = s;
        }
    }
    for (int w = 0; w < 2; w++) {
        uint32_t p = c->nvic_pend[w] & c->nvic_en[w];
        while (p) {
            unsigned i = (unsigned)(w * 32 + __builtin_ctz(p));
            p &= p - 1;
            bool s = exc_targets_secure(c, 16 + i, true);
            int q = exc_priority(c, 16 + i, s, false);
            if (q < bp) {
                bp = q;
                best = 16 + i;
                bs = s;
            }
        }
    }
    *secure = bs;
    *prio = best ? exc_priority(c, best, bs, true) : bp;
    return best;
}

// The pending exception to take next and the state it goes to, with its group priority in *prio.
// It runs whenever a core looks, so Secure interrupts alone take a short path.
static unsigned best_pending(cpu_t *c, int *prio, bool *secure)
{
    uint32_t act[2];
    active_irq_mask(c, act);
    uint64_t lines = (c->m->irq_ext | c->m->irq_core_ext[c->id]);
    c->nvic_pend[0] |= (uint32_t)lines & ~act[0];
    c->nvic_pend[1] |= (uint32_t)(lines >> 32) & ~act[1];
    if (c->pend_nmi) {
        *prio = -2;
        *secure = exc_targets_secure(c, EXC_NMI, true);
        return EXC_NMI;
    }
    _Static_assert(offsetof(cpu_t, pend_systick_ns) == offsetof(cpu_t, pend_pendsv) + 3, "flags must be adjacent");
    uint32_t banked;
    memcpy(&banked, &c->pend_pendsv, sizeof(banked));
    if (unlikely(banked | c->nvic_itns[0] | c->nvic_itns[1] | ((c->aircr | c->aircr_ns) & 0x400)))
        return best_pending_full(c, prio, secure);
    unsigned best = 0;
    int bp = 1000;
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
    *secure = true;
    *prio = bp;
    return best;
}

bool cpu_irq_wakes(cpu_t *c)
{
    int bp;
    bool s;
    unsigned n = best_pending(c, &bp, &s);
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

// PushStack() and ExceptionTaken(): the frame goes on the interrupted state's stack, and an
// exception that leaves Secure state for a Non-secure handler also stacks r4-r11 under an integrity
// signature and clears them, so the handler sees none of the Secure state.
static void exception_entry(cpu_t *c, unsigned n, bool to_secure, uint32_t ret)
{
    bool from_secure = !c->ns, handler = c->ipsr != 0;
    save_sp(c);
    uint32_t *spp = sp_slot(c, from_secure, use_psp(c));
    bool fp = c->v8 && (c->control & 4) && (from_secure || (c->nsacr & (1u << 10)));
    bool fp_callee = fp && from_secure && (c->fpccr & FPCCR_TS);
    uint32_t sp = *spp;
    uint32_t frame_size = 0x20 + (fp ? 0x48 : 0) + (fp_callee ? 0x40 : 0);
    uint32_t align = (sp & 4) ? 1 : 0;
    sp = (sp - frame_size) & ~7u;
    uint32_t psr = xpsr_value(c) | (align << 9) | (from_secure && (c->control & CONTROL_SFPA) ? 1u << 20 : 0);
    uint32_t frame[8] = {c->r[0], c->r[1], c->r[2], c->r[3], c->r[12], c->r[14], ret, psr};
    for (int i = 0; i < 8; i++)
        st32(c, sp + 4u * i, frame[i]);
    if (fp) {
        for (int i = 0; i < 16; i++)
            st32(c, sp + 0x20 + 4u * i, c->s[i]);
        st32(c, sp + 0x60, c->fpscr);
        if (fp_callee)
            for (int i = 0; i < 16; i++)
                st32(c, sp + 0x68 + 4u * i, c->s[16 + i]);
    }
    uint32_t exc_ret = 0xffffff80u | EXC_RETURN_DCRS | (from_secure ? EXC_RETURN_S : 0) |
                       (fp ? 0 : EXC_RETURN_FTYPE) | (handler ? 0 : EXC_RETURN_MODE);
    if (from_secure && !to_secure) {
        sp -= 0x28;
        st32(c, sp, 0xfefa125au | (fp ? 0 : 1));
        for (int i = 0; i < 8; i++)
            st32(c, sp + 8 + 4u * i, c->r[4 + i]);
    }
    *spp = sp;
    if (to_secure)
        exc_ret |= EXC_RETURN_ES;
    if (*control_reg(c, to_secure) & 2)
        exc_ret |= EXC_RETURN_SPSEL;
    c->r[14] = exc_ret;
    if (!to_secure) {
        for (int i = 0; i < 4; i++)
            c->r[i] = 0;
        c->r[12] = 0;
        c->apsr = 0;
        if (from_secure)
            for (int i = 4; i < 12; i++)
                c->r[i] = 0;
    }
    if (to_secure == c->ns)
        switch_state(c, !to_secure);
    c->ipsr = n;
    c->itstate = 0;
    *control_reg(c, to_secure) &= ~2u;
    c->control &= ~(4u | CONTROL_SFPA);
    set_active(c, n, to_secure, true);
    c->r[13] = *sp_slot(c, to_secure, false);
    c->excl_valid = false;
    c->fault = false;
    c->fault_exc = 0;
    uint32_t addr = (to_secure ? c->vtor : c->vtor_ns) + 4 * n;
    uint32_t lo, hi;
    // A Non-secure vector table in Secure memory is a HardFault for Secure state to handle.
    bool denied = !to_secure && !security_check(c, addr, false, false, &lo, &hi).ns;
    uint32_t vec = denied ? 0 : ld32(c, addr);
    if (c->fault || denied || !(vec & 1)) {
        c->fault = false;
        c->fault_exc = 0;
        if (n == EXC_HARDFAULT || n == EXC_NMI) {
            lockup(c, "bad vector");
            return;
        }
        c->hfsr |= 2;
        set_active(c, n, to_secure, false);
        exception_entry(c, EXC_HARDFAULT, denied || exc_targets_secure(c, EXC_HARDFAULT, to_secure), ret);
        return;
    }
    c->r[15] = vec & ~1u;
    c->cyc += 12;
    c->check_exc = true;
}

static void raise_fault(cpu_t *c, unsigned exc, bool secure, uint32_t pc, const char *why);
static void take_fault(cpu_t *c, unsigned exc, uint32_t pc, const char *why);

// DeActivate(): a negative raw priority means the return is from NMI or HardFault whatever IPSR
// says, and FAULTMASK survives only those returns.
static void deactivate(cpu_t *c, unsigned n, bool secure)
{
    int raw = raw_exec_priority(c);
    if (raw == -1)
        set_active(c, EXC_HARDFAULT, !bfhfnmins(c), false);
    else if (raw == -2)
        set_active(c, EXC_NMI, !bfhfnmins(c), false);
    else if (raw == -3)
        set_active(c, EXC_HARDFAULT, true, false);
    else
        set_active(c, n, secure, false);
    if (raw >= 0)
        *(secure ? &c->faultmask : &c->faultmask_ns) = 0;
}

// ExceptionReturn() and PopStack(), without tail-chaining: the next exception is taken from the
// state returned to.
static void exception_return(cpu_t *c, uint32_t exc_ret, uint32_t pc)
{
    unsigned n = c->ipsr;
    if ((exc_ret & 0x00ffff82u) != 0x00ffff80u) {
        lockup(c, "bad EXC_RETURN");
        return;
    }
    bool es = (exc_ret & EXC_RETURN_ES) || !c->v8, to_secure = (exc_ret & EXC_RETURN_S) || !c->v8;
    bool to_thread = exc_ret & EXC_RETURN_MODE, ftype = (exc_ret & EXC_RETURN_FTYPE) || !c->v8;
    unsigned err = 0;
    if (c->v8 && c->ns && es) {
        es = false;
        err = EXC_SECUREFAULT;
    }
    if (c->v8 && !es && !(exc_ret & EXC_RETURN_DCRS))
        err = EXC_SECUREFAULT;
    if (!err && !exc_active_for(c, n, es))
        err = EXC_USAGEFAULT;
    uint32_t *ctl = control_reg(c, es);
    *ctl = (*ctl & ~2u) | (exc_ret & EXC_RETURN_SPSEL ? 2u : 0);
    deactivate(c, n, es);
    if (err) {
        if (err == EXC_SECUREFAULT)
            c->sfsr |= SFSR_INVER;
        else
            ufsr_set(c, UFSR_INVPC);
        take_fault(c, err, pc, "bad exception return");
        return;
    }
    save_sp(c);
    if (to_secure == c->ns)
        switch_state(c, !to_secure);
    uint32_t *spp = sp_slot(c, to_secure, to_thread && (*control_reg(c, to_secure) & 2));
    uint32_t sp = *spp;
    // The callee frame is there if the exception left Secure state for a Non-secure handler.
    if (to_secure && (!es || !(exc_ret & EXC_RETURN_DCRS))) {
        if (ld32(c, sp) != (0xfefa125au | (ftype ? 1 : 0))) {
            c->sfsr |= SFSR_INVIS;
            c->ipsr = to_thread ? 0 : c->ipsr;
            load_sp(c);
            take_fault(c, EXC_SECUREFAULT, pc, "bad integrity signature");
            return;
        }
        for (int i = 0; i < 8; i++)
            c->r[4 + i] = ld32(c, sp + 8 + 4u * i);
        sp += 0x28;
    }
    uint32_t frame[8];
    for (int i = 0; i < 8; i++)
        frame[i] = ld32(c, sp + 4u * i);
    if (!ftype) {
        for (int i = 0; i < 16; i++)
            c->s[i] = ld32(c, sp + 0x20 + 4u * i);
        c->fpscr = ld32(c, sp + 0x60);
        if (to_secure && (c->fpccr & FPCCR_TS)) {
            for (int i = 0; i < 16; i++)
                c->s[16 + i] = ld32(c, sp + 0x68 + 4u * i);
            sp += 0x40;
        }
        sp += 0x68;
        c->control |= 4u;
    } else {
        sp += 0x20;
        c->control &= ~4u;
    }
    if (frame[7] & (1u << 9))
        sp |= 4;
    *spp = sp;
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
    if (to_secure && c->v8)
        c->control = (c->control & ~CONTROL_SFPA) | (frame[7] & (1u << 20) ? CONTROL_SFPA : 0);
    c->excl_valid = false;
    load_sp(c);
    c->cyc += 12;
    c->check_exc = true;
    if (c->v8 && to_thread == ((frame[7] & 0x1ff) != 0)) {
        ufsr_set(c, UFSR_INVPC);
        take_fault(c, EXC_USAGEFAULT, c->r[15], "exception frame for the wrong mode");
    }
}

// CreateException() and HandleException() for a synchronous fault going to the given state.
static void raise_fault(cpu_t *c, unsigned exc, bool secure, uint32_t pc, const char *why)
{
    c->fault = false;
    c->fault_exc = 0;
    if (c->m->trace)
        emu_log("core %d %s at %08x", c->id, why, pc);
    if (c->v8) {
        bool enabled;
        switch (exc) {
        case EXC_MEMMANAGE: enabled = (secure ? c->shcsr : c->shcsr_ns) & (1u << 16); break;
        case EXC_BUSFAULT: enabled = c->shcsr & (1u << 17); break;
        case EXC_USAGEFAULT: enabled = (secure ? c->shcsr : c->shcsr_ns) & (1u << 18); break;
        case EXC_SECUREFAULT: enabled = c->shcsr & (1u << 19); break;
        default: enabled = true; break;
        }
        if (exc != EXC_HARDFAULT && (!enabled || exc_priority(c, exc, secure, true) >= exec_priority(c, true))) {
            c->hfsr |= 1u << 30;
            exc = EXC_HARDFAULT;
            secure = exc_targets_secure(c, EXC_HARDFAULT, secure);
        }
    } else {
        exc = EXC_HARDFAULT;
    }
    if (exc == EXC_HARDFAULT && exc_priority(c, exc, secure, true) >= exec_priority(c, true)) {
        c->r[15] = pc;
        lockup(c, why);
        return;
    }
    exception_entry(c, exc, secure, pc);
}

static void take_fault(cpu_t *c, unsigned exc, uint32_t pc, const char *why)
{
    raise_fault(c, exc, exc_targets_secure(c, exc, !c->ns), pc, why);
}

// Takes the fault an access or check flagged while running the instruction at pc, whose IT state
// was it.
static void take_flagged_fault(cpu_t *c, uint32_t pc, uint32_t it)
{
    unsigned exc = c->fault_exc ? c->fault_exc : EXC_BUSFAULT;
    c->r[15] = pc;
    c->itstate = it;
    take_fault(c, exc, pc,
               exc == EXC_USAGEFAULT ? "unaligned access" : exc == EXC_SECUREFAULT ? "security violation" : "bus fault");
}

static void undefined(cpu_t *c, uint32_t pc, uint32_t op)
{
    ufsr_set(c, UFSR_UNDEFINSTR);
    if (c->m->trace)
        emu_log("core %d undefined instruction %08x at %08x", c->id, op, pc);
    take_fault(c, EXC_USAGEFAULT, pc, "undefined instruction");
}

void cpu_undefined(cpu_t *c, uint32_t pc, uint32_t op)
{
    undefined(c, pc, op);
}

// FunctionReturn(): Non-secure code returning to the Secure caller of BLXNS, which left the return
// address and the caller's IPSR on its stack.
static void function_return(cpu_t *c, uint32_t pc)
{
    bool handler = c->ipsr != 0;
    uint32_t *spp = sp_slot(c, true, !handler && (c->control & 2));
    uint32_t sp = *spp;
    save_sp(c);
    switch_state(c, false);
    uint32_t psr = ld32(c, sp + 4), target = ld32(c, sp);
    unsigned exc = psr & 0x1ff;
    if (c->fault || !((c->ipsr == 0 && exc == 0) || (c->ipsr == 1 && exc != 0))) {
        switch_state(c, true);
        load_sp(c);
        if (!c->fault) {
            c->cfsr |= UFSR_INVPC;
            raise_fault(c, EXC_USAGEFAULT, true, pc, "bad function return");
        }
        return;
    }
    *spp = sp + 8;
    c->ipsr = exc;
    c->control = (c->control & ~CONTROL_SFPA) | (psr & (1u << 20) ? CONTROL_SFPA : 0);
    c->itstate = 0;
    load_sp(c);
    if (!(target & 1)) {
        c->cfsr |= UFSR_INVSTATE;
        c->r[15] = target & ~1u;
        take_fault(c, EXC_USAGEFAULT, target & ~1u, "branch to ARM state");
        return;
    }
    c->r[15] = target & ~1u;
}

static void branch_special(cpu_t *c, uint32_t v, uint32_t pc_of_insn)
{
    if (c->v8 && (v | 1) == FNC_RETURN)
        function_return(c, pc_of_insn);
    else
        exception_return(c, v, pc_of_insn);
}

// BranchReturn(): the targets at the top of the address map are function and exception returns.
static inline void branch_interworking(cpu_t *c, uint32_t v, uint32_t pc_of_insn)
{
    if (unlikely(v >= 0xfe000000u) && ((c->v8 && (v | 1) == FNC_RETURN) || (c->ipsr && (v >> 24) == 0xff))) {
        branch_special(c, v, pc_of_insn);
        return;
    }
    if (!(v & 1)) {
        ufsr_set(c, UFSR_INVSTATE);
        c->r[15] = v & ~1u;
        take_fault(c, EXC_USAGEFAULT, v & ~1u, "branch to ARM state");
        return;
    }
    c->r[15] = v & ~1u;
}

// BXNS and BLXNS, which only Secure state has: a target with bit 0 clear is in Non-secure state,
// and BLXNS leaves its return address and IPSR on the Secure stack for FunctionReturn().
static void branch_nonsecure(cpu_t *c, uint32_t pc, uint16_t op, uint32_t target)
{
    if (c->ns || !c->v8) {
        undefined(c, pc, op);
        return;
    }
    bool link = op & 0x80;
    if (!link && unlikely(target >= 0xfe000000u) &&
        ((target | 1) == FNC_RETURN || (c->ipsr && (target >> 24) == 0xff))) {
        branch_special(c, target, pc);
        return;
    }
    if (target & 1) {
        if (link)
            c->r[14] = (pc + 2) | 1;
        c->r[15] = target & ~1u;
        return;
    }
    if (link) {
        uint32_t sp = c->r[13] - 8;
        st32(c, sp, (pc + 2) | 1);
        st32(c, sp + 4, c->ipsr | (c->control & CONTROL_SFPA ? 1u << 20 : 0));
        if (c->fault)
            return;
        c->r[13] = sp;
        c->r[14] = FNC_RETURN;
        if (c->ipsr)
            c->ipsr = 1;
    }
    c->control &= ~CONTROL_SFPA;
    set_state(c, true);
    c->r[15] = target;
}

// Checks a fetch outside the window checked last, entering Secure state at a secure gateway (SG
// in Non-secure-callable memory) and returning whether that or a fault dealt with the instruction.
static __attribute__((noinline)) bool fetch_check(cpu_t *c, uint32_t pc)
{
    uint32_t lo = 0, hi = 0xffffffffu;
    sattr_t a = {.ns = c->ns};
    if (c->v8)
        a = security_check(c, pc, true, !c->ns, &lo, &hi);
    if (a.ns == c->ns) {
        c->fw_lo = lo;
        c->fw_size = hi - lo;
        return false;
    }
    if (c->ns) {
        uint32_t lo2, hi2;
        if (a.nsc && !security_check(c, pc + 2, true, false, &lo2, &hi2).ns && fetch16(c, pc) == 0xe97f &&
            fetch16(c, pc + 2) == 0xe97f) {
            c->r[14] &= ~1u;
            c->control &= ~CONTROL_SFPA;
            set_state(c, false);
            c->itstate = 0;
            c->r[15] = pc + 4;
            c->cyc++;
            return true;
        }
        c->sfsr |= SFSR_INVEP;
    } else {
        c->sfsr |= SFSR_INVTRAN;
    }
    take_fault(c, EXC_SECUREFAULT, pc, c->ns ? "Non-secure entry to Secure code" : "Secure fetch from Non-secure memory");
    return true;
}

// The second halfword of a 32-bit instruction must have the same attribution as the first.
static __attribute__((noinline)) bool fetch_check_hw2(cpu_t *c, uint32_t pc)
{
    uint32_t lo, hi;
    if (security_check(c, pc + 2, true, !c->ns, &lo, &hi).ns == c->ns)
        return false;
    c->sfsr |= c->ns ? SFSR_INVEP : SFSR_INVTRAN;
    take_fault(c, EXC_SECUREFAULT, pc, "instruction straddles a security boundary");
    return true;
}

static void check_exceptions(cpu_t *c)
{
    c->check_exc = false;
    int bp;
    bool secure;
    unsigned n = best_pending(c, &bp, &secure);
    if (!n || bp >= exec_priority(c, true))
        return;
    if (n == EXC_NMI)
        c->pend_nmi = false;
    else if (n == EXC_PENDSV)
        *(secure ? &c->pend_pendsv : &c->pend_pendsv_ns) = false;
    else if (n == EXC_SYSTICK)
        *(secure ? &c->pend_systick : &c->pend_systick_ns) = false;
    else
        c->nvic_pend[(n - 16) >> 5] &= ~(1u << ((n - 16) & 31));
    if (c->state == CPU_WFI || c->state == CPU_WFE)
        c->state = CPU_RUN;
    exception_entry(c, n, secure, c->r[15]);
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
        } else if ((c->scr | c->scr_ns) & 0x10) {
            uint32_t act[2];
            active_irq_mask(c, act);
            uint64_t lines = (c->m->irq_ext | c->m->irq_core_ext[c->id]);
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
    ev_cancel(m, &c->syst[0].ev);
    ev_cancel(m, &c->syst[1].ev);
    uint64_t t = c->t;
    memset(c, 0, offsetof(cpu_t, m));
    c->id = id;
    c->v8 = v8;
    c->t = t;
    c->rpage = m->rpage;
    c->wpage = m->wpage;
    c->pri_mask = v8 ? 0xf0 : 0xc0;
    c->ccr = c->ccr_ns = v8 ? 0x201 : 0x204;
    c->syst[0].ev.ctx = c->syst[1].ev.ctx = c;
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
// SysTick: one for each security state.

static void systick_fire(chip_t *m, emu_event_t *ev);

static uint64_t systick_period(cpu_t *c, int bank)
{
    // CLKSOURCE=0 selects the core's tick from the TICKS block, nominally 1 MHz.
    return (c->syst[bank].csr & 4) ? c->m->sys_ps : PS_PER_US;
}

static uint64_t cpu_now(cpu_t *c)
{
    return c->t > c->m->now ? c->t : c->m->now;
}

static uint32_t systick_current(cpu_t *c, int bank)
{
    systick_t *st = &c->syst[bank];
    if (!(st->csr & 1))
        return st->cvr;
    uint64_t ticks = (cpu_now(c) - st->t) / systick_period(c, bank);
    uint64_t reload = (uint64_t)(st->rvr & 0xffffff) + 1;
    if (ticks <= st->cvr)
        return (uint32_t)(st->cvr - ticks);
    ticks -= st->cvr + 1;
    st->countflag = true;
    return (uint32_t)(st->rvr - (ticks % reload));
}

static void systick_schedule(cpu_t *c, int bank)
{
    systick_t *st = &c->syst[bank];
    ev_cancel(c->m, &st->ev);
    if (!(st->csr & 1))
        return;
    uint64_t now = cpu_now(c);
    st->ev.fn = systick_fire;
    st->ev.ctx = c;
    ev_schedule(c->m, &st->ev, now + ((uint64_t)st->cvr + 1) * systick_period(c, bank));
}

static void systick_fire(chip_t *m, emu_event_t *ev)
{
    (void)m;
    cpu_t *c = ev->ctx;
    int bank = ev == &c->syst[1].ev;
    systick_t *st = &c->syst[bank];
    st->countflag = true;
    if (st->csr & 2) {
        *(bank ? &c->pend_systick_ns : &c->pend_systick) = true;
        c->check_exc = true;
    }
    st->cvr = st->rvr & 0xffffff;
    st->t = ev->when;
    if ((st->csr & 1) && st->rvr) {
        st->ev.fn = systick_fire;
        ev_schedule(c->m, &st->ev, ev->when + ((uint64_t)st->cvr + 1) * systick_period(c, bank));
    }
}

static uint32_t systick_read(cpu_t *c, int bank, uint32_t off)
{
    systick_t *st = &c->syst[bank];
    switch (off) {
    case 0xe010: {
        systick_current(c, bank);
        uint32_t v = (st->csr & 7) | (st->countflag ? (1u << 16) : 0);
        st->countflag = false;
        return v;
    }
    case 0xe014: return st->rvr & 0xffffff;
    case 0xe018: return systick_current(c, bank);
    default: return 0x40000000u | 9999;
    }
}

static void systick_write(cpu_t *c, int bank, uint32_t off, uint32_t v)
{
    systick_t *st = &c->syst[bank];
    switch (off) {
    case 0xe010:
        st->cvr = systick_current(c, bank);
        st->t = cpu_now(c);
        st->csr = v & 7;
        systick_schedule(c, bank);
        break;
    case 0xe014:
        st->rvr = v & 0xffffff;
        break;
    case 0xe018:
        st->cvr = 0;
        st->countflag = false;
        st->t = cpu_now(c);
        systick_schedule(c, bank);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------------------------
// System control space
//
// Secure state reaches the Non-secure registers through the alias at 0xe002e000, which reads as
// zero to Non-secure state, and registers banked by field show Non-secure state the fields it may use.

static bool mpu_check(const cpu_t *c, uint32_t a, bool priv, bool secure, unsigned *ap, unsigned *region,
                      bool *region_valid)
{
    const mpu_t *u = &c->mpu[secure ? 0 : 1];
    *ap = 1;
    *region = 0;
    *region_valid = false;
    if ((a >> 20) == 0xe00 || !(u->ctrl & 1) || (!(u->ctrl & 2) && exec_priority(c, true) < 0))
        return true;
    bool hit = (u->ctrl & 4) && priv, matched = false;
    for (unsigned i = 0; i < 8; i++) {
        uint32_t rbar = u->rbar[i], rlar = u->rlar[i];
        if (!(rlar & 1) || a < (rbar & ~0x1fu) || a > (rlar | 0x1fu))
            continue;
        if (matched) {
            *region_valid = false;
            *region = 0;
            hit = false;
        } else {
            matched = true;
            *ap = (rbar >> 1) & 3;
            *region = i;
            *region_valid = true;
            hit = true;
        }
    }
    return hit;
}

// TTResp(): TT, TTT, TTA and TTAT.
static uint32_t tt_resp(cpu_t *c, uint32_t a, bool alt, bool force_unpriv)
{
    uint32_t resp = 0;
    bool secure = !c->ns, addr_secure = false;
    if (secure) {
        uint32_t lo, hi;
        sattr_t s = security_check(c, a, false, true, &lo, &hi);
        if (s.srvalid)
            resp |= (uint32_t)s.sregion << 8 | 1u << 17;
        if (s.irvalid)
            resp |= (uint32_t)s.iregion << 24 | 1u << 23;
        addr_secure = !s.ns;
        if (addr_secure)
            resp |= 1u << 22;
    }
    if (privileged(c, secure) || alt) {
        bool mpu_secure = alt != secure;
        bool priv = !force_unpriv && privileged(c, mpu_secure);
        unsigned ap, region;
        bool region_valid;
        bool read = false, write = false;
        if (mpu_check(c, a, priv, mpu_secure, &ap, &region, &region_valid)) {
            read = priv || (ap & 1);
            write = !(ap & 2) && (priv || (ap & 1));
        }
        if (region_valid)
            resp |= region | 1u << 16;
        resp |= (read ? 1u << 18 : 0) | (write ? 1u << 19 : 0);
        if (secure && !addr_secure)
            resp |= (read ? 1u << 20 : 0) | (write ? 1u << 21 : 0);
    }
    return resp;
}

// MPU_RBAR and MPU_RLAR at 0xed9c and 0xeda0 reach the region RNR selects; on Armv8-M the three
// aliases after them reach the next three in the same group of four. On Armv6-M the second is
// RASR, and writing RBAR with VALID set selects the region.
static uint32_t mpu_region_reg(cpu_t *c, mpu_t *u, uint32_t off, uint32_t v, bool write)
{
    unsigned n = (off - 0xed9c) >> 3;
    if (!c->v8 && n)
        return 0;
    if (!c->v8 && write && off == 0xed9c && (v & 0x10))
        u->rnr = v & 7;
    unsigned region = (n ? (u->rnr & ~3u) + n : u->rnr) & 7;
    uint32_t *reg = (off - 0xed9c) & 4 ? &u->rlar[region] : &u->rbar[region];
    if (write)
        *reg = (off - 0xed9c) & 4 ? (c->v8 ? v & ~0x10u : v) : (c->v8 ? v : v & ~0x1fu);
    return !c->v8 && !((off - 0xed9c) & 4) ? *reg | (u->rnr & 0xf) : *reg;
}

// The interrupts a state's view of the NVIC covers: all of them for Secure state.
static inline uint32_t nvic_view(const cpu_t *c, bool ns, unsigned w)
{
    return ns ? c->nvic_itns[w] : 0xffffffffu;
}

static uint32_t scs_read(cpu_t *c, uint32_t off, bool ns)
{
    bool bfh = bfhfnmins(c);
    switch (off) {
    case 0xe004: return 1;
    case 0xe010:
    case 0xe014:
    case 0xe018:
    case 0xe01c: return systick_read(c, ns, off);
    case 0xed00: return c->v8 ? 0x411fd210u : 0x410cc601u;
    case 0xed04: {
        int bp;
        bool s;
        unsigned pend = best_pending(c, &bp, &s);
        uint32_t v = c->ipsr | (pend << 12);
        if (ns ? c->pend_pendsv_ns : c->pend_pendsv)
            v |= 1u << 28;
        if (ns ? c->pend_systick_ns : c->pend_systick)
            v |= 1u << 26;
        if (c->pend_nmi && (!ns || bfh))
            v |= 1u << 31;
        if ((c->nvic_pend[0] & c->nvic_en[0]) | (c->nvic_pend[1] & c->nvic_en[1]))
            v |= 1u << 22;
        return v;
    }
    case 0xed08: return ns ? c->vtor_ns : c->vtor;
    case 0xed0c:
        if (ns)
            return 0xfa050000u | (c->aircr_ns & 0x700) | (c->aircr & AIRCR_BFHFNMINS);
        return 0xfa050000u | (c->aircr & (0x700 | AIRCR_PRIS | AIRCR_BFHFNMINS | AIRCR_SYSRESETREQS));
    case 0xed10: return ns ? (c->scr_ns & 0x12) | (c->scr & 4) : c->scr;
    case 0xed14: return ns ? c->ccr_ns : c->ccr;
    case 0xed18: {
        const uint8_t *p = ns ? c->shpr_ns : c->shpr;
        uint32_t bus = ns && !bfh ? 0 : c->shpr[5], secf = ns ? 0 : c->shpr[7];
        return p[4] | (bus << 8) | (p[6] << 16) | (secf << 24);
    }
    case 0xed1c: return (uint32_t)(ns ? c->shpr_ns : c->shpr)[11] << 24;
    case 0xed20: {
        const uint8_t *p = ns ? c->shpr_ns : c->shpr;
        return ((uint32_t)p[15] << 24) | ((uint32_t)p[14] << 16) | (ns ? 0 : c->shpr[12]);
    }
    case 0xed24: {
        bool s = !ns;
        uint32_t v = (ns ? c->shcsr_ns & 0x50000u : c->shcsr & 0xf0000u) | (ns && bfh ? c->shcsr & 0x20000u : 0);
        if (exc_active_for(c, EXC_MEMMANAGE, s))
            v |= 1;
        if (exc_active(c, EXC_BUSFAULT) && (s || bfh))
            v |= 2;
        if (exc_active_for(c, EXC_HARDFAULT, s) && c->v8)
            v |= 4;
        if (exc_active_for(c, EXC_USAGEFAULT, s))
            v |= 8;
        if (s && exc_active(c, EXC_SECUREFAULT))
            v |= 0x10;
        if (exc_active(c, EXC_NMI) && (s || bfh) && c->v8)
            v |= 0x20;
        if (exc_active_for(c, EXC_SVC, s))
            v |= 1u << 7;
        if (exc_active_for(c, EXC_PENDSV, s))
            v |= 1u << 10;
        if (exc_active_for(c, EXC_SYSTICK, s))
            v |= 1u << 11;
        return v;
    }
    case 0xed28: return ns ? (c->cfsr_ns & 0xffff00ffu) | (bfh ? c->cfsr & 0xff00u : 0) : c->cfsr;
    case 0xed2c: return c->hfsr;
    case 0xed34: return ns ? c->mmfar_ns : c->mmfar;
    case 0xed38: return ns && !bfh ? 0 : c->bfar;
    case 0xed88: return ns ? c->cpacr_ns : c->cpacr;
    case 0xed8c: return c->nsacr;
    case 0xed90: return 8u << 8;
    case 0xed94: return c->mpu[ns].ctrl;
    case 0xed98: return c->mpu[ns].rnr;
    case 0xedc0: return c->mpu[ns].mair[0];
    case 0xedc4: return c->mpu[ns].mair[1];
    case 0xedd0: return c->v8 && !ns ? c->sau_ctrl : 0;
    case 0xedd4: return c->v8 && !ns ? 8 : 0;
    case 0xedd8: return c->v8 && !ns ? c->sau_rnr : 0;
    case 0xeddc: return c->v8 && !ns ? c->sau_rbar[c->sau_rnr & 7] : 0;
    case 0xede0: return c->v8 && !ns ? c->sau_rlar[c->sau_rnr & 7] : 0;
    case 0xede4: return c->v8 && !ns ? c->sfsr : 0;
    case 0xede8: return c->v8 && !ns ? c->sfar : 0;
    case 0xef34: return ns ? c->fpccr_ns : c->fpccr;
    case 0xef38: return ns ? c->fpcar_ns : c->fpcar;
    case 0xef3c: return ns ? c->fpdscr_ns : c->fpdscr;
    default:
        if (off >= 0xed9c && off < 0xedc0)
            return mpu_region_reg(c, &c->mpu[ns], off, 0, false);
        if (off >= 0xe100 && off < 0xe108)
            return c->nvic_en[(off - 0xe100) >> 2] & nvic_view(c, ns, (off - 0xe100) >> 2);
        if (off >= 0xe180 && off < 0xe188)
            return c->nvic_en[(off - 0xe180) >> 2] & nvic_view(c, ns, (off - 0xe180) >> 2);
        if ((off >= 0xe200 && off < 0xe208) || (off >= 0xe280 && off < 0xe288)) {
            int bp;
            bool s;
            best_pending(c, &bp, &s);
            return c->nvic_pend[(off & 0x7f) >> 2] & nvic_view(c, ns, (off & 0x7f) >> 2);
        }
        if (off >= 0xe300 && off < 0xe308) {
            uint32_t act[2];
            active_irq_mask(c, act);
            return act[(off - 0xe300) >> 2] & nvic_view(c, ns, (off - 0xe300) >> 2);
        }
        if (off >= 0xe380 && off < 0xe388)
            return c->v8 && !ns ? c->nvic_itns[(off - 0xe380) >> 2] : 0;
        if (off >= 0xe400 && off < 0xe440) {
            unsigned i = off - 0xe400;
            uint32_t v = 0;
            for (unsigned k = 0; k < 4; k++)
                if (!ns || exc_targets_secure(c, 16 + i + k, true) == false)
                    v |= (uint32_t)c->nvic_pri[i + k] << (8 * k);
            return v;
        }
        return 0;
    }
}

// SAU changes can make Secure code that has been translated Non-secure, which it may not execute.
static void sau_changed(cpu_t *c)
{
    c->fw_size = c->dw_size = 0;
    c->m->code_flush = true;
    c->m->core[0].check_exc = c->m->core[1].check_exc = true;
}

static void scs_write(cpu_t *c, uint32_t off, uint32_t v, bool ns)
{
    chip_t *m = c->m;
    bool bfh = bfhfnmins(c);
    switch (off) {
    case 0xe010:
    case 0xe014:
    case 0xe018: systick_write(c, ns, off, v); break;
    case 0xed04:
        if ((v & (1u << 31)) && (!ns || bfh))
            c->pend_nmi = true;
        if (v & (1u << 28))
            *(ns ? &c->pend_pendsv_ns : &c->pend_pendsv) = true;
        if (v & (1u << 27))
            *(ns ? &c->pend_pendsv_ns : &c->pend_pendsv) = false;
        if (v & (1u << 26))
            *(ns ? &c->pend_systick_ns : &c->pend_systick) = true;
        if (v & (1u << 25))
            *(ns ? &c->pend_systick_ns : &c->pend_systick) = false;
        c->check_exc = true;
        break;
    case 0xed08:
        *(ns ? &c->vtor_ns : &c->vtor) = v & 0xffffff80u;
        break;
    case 0xed0c:
        if ((v >> 16) != 0x05fa)
            break;
        if (ns) {
            c->aircr_ns = v & 0x700;
            if ((v & 4) && !(c->aircr & AIRCR_SYSRESETREQS))
                chip_request(m, REQ_RESET, "SYSRESETREQ");
        } else {
            uint32_t keep = c->v8 ? 0x700 | AIRCR_PRIS | AIRCR_BFHFNMINS | AIRCR_SYSRESETREQS : 0x700;
            c->aircr = v & keep;
            if (v & 4)
                chip_request(m, REQ_RESET, "SYSRESETREQ");
        }
        c->check_exc = true;
        break;
    case 0xed10:
        if (ns) {
            c->scr_ns = v & 0x12;
            c->scr = (c->scr & ~4u) | (v & 4);
        } else {
            c->scr = v & 0x1e;
        }
        break;
    case 0xed14: {
        uint32_t *ccr = ns ? &c->ccr_ns : &c->ccr;
        *ccr = (*ccr & ~0x31au) | (v & 0x31au);
        break;
    }
    case 0xed18: {
        uint8_t *p = ns ? c->shpr_ns : c->shpr;
        p[4] = (uint8_t)(v & c->pri_mask);
        p[6] = (uint8_t)((v >> 16) & c->pri_mask);
        if (!ns || bfh)
            c->shpr[5] = (uint8_t)((v >> 8) & c->pri_mask);
        if (!ns)
            c->shpr[7] = (uint8_t)((v >> 24) & c->pri_mask);
        c->check_exc = true;
        break;
    }
    case 0xed1c:
        (ns ? c->shpr_ns : c->shpr)[11] = (uint8_t)((v >> 24) & c->pri_mask);
        c->check_exc = true;
        break;
    case 0xed20: {
        uint8_t *p = ns ? c->shpr_ns : c->shpr;
        if (!ns)
            c->shpr[12] = (uint8_t)(v & c->pri_mask);
        p[14] = (uint8_t)((v >> 16) & c->pri_mask);
        p[15] = (uint8_t)((v >> 24) & c->pri_mask);
        c->check_exc = true;
        break;
    }
    case 0xed24:
        if (ns) {
            c->shcsr_ns = v & 0x50000u;
            if (bfh)
                c->shcsr = (c->shcsr & ~0x20000u) | (v & 0x20000u);
        } else {
            c->shcsr = v & 0x000f0000u;
        }
        break;
    case 0xed28:
        if (ns) {
            c->cfsr_ns &= ~(v & 0xffff00ffu);
            if (bfh)
                c->cfsr &= ~(v & 0xff00u);
        } else {
            c->cfsr &= ~v;
        }
        break;
    case 0xed2c:
        c->hfsr &= ~v;
        break;
    case 0xed88:
        *(ns ? &c->cpacr_ns : &c->cpacr) = v;
        break;
    case 0xed8c:
        if (!ns)
            c->nsacr = v;
        break;
    case 0xed94:
        c->mpu[ns].ctrl = v & 7;
        break;
    case 0xed98:
        // Only the bits that number the eight regions exist, as the A4 boot ROM checks.
        c->mpu[ns].rnr = v & (c->v8 ? 7 : 0xff);
        break;
    case 0xedc0:
    case 0xedc4:
        c->mpu[ns].mair[(off - 0xedc0) >> 2] = v;
        break;
    case 0xedd0:
    case 0xedd8:
    case 0xeddc:
    case 0xede0:
        if (!c->v8 || ns)
            break;
        if (off == 0xedd0)
            c->sau_ctrl = v & 3;
        else if (off == 0xedd8)
            c->sau_rnr = v & 7;
        else if (off == 0xeddc)
            c->sau_rbar[c->sau_rnr & 7] = v & ~0x1fu;
        else
            c->sau_rlar[c->sau_rnr & 7] = v & ~0x1cu;
        sau_changed(c);
        break;
    case 0xede4:
        if (!ns)
            c->sfsr &= ~v;
        break;
    case 0xede8:
        if (!ns)
            c->sfar = v;
        break;
    case 0xef34:
        *(ns ? &c->fpccr_ns : &c->fpccr) = v;
        break;
    case 0xef38:
        *(ns ? &c->fpcar_ns : &c->fpcar) = v;
        break;
    case 0xef3c:
        *(ns ? &c->fpdscr_ns : &c->fpdscr) = v;
        break;
    default:
        if (off >= 0xed9c && off < 0xedc0) {
            mpu_region_reg(c, &c->mpu[ns], off, v, true);
            break;
        }
        if (off >= 0xe100 && off < 0xe108) {
            c->nvic_en[(off - 0xe100) >> 2] |= v & nvic_view(c, ns, (off - 0xe100) >> 2);
            c->check_exc = true;
        } else if (off >= 0xe180 && off < 0xe188) {
            c->nvic_en[(off - 0xe180) >> 2] &= ~(v & nvic_view(c, ns, (off - 0xe180) >> 2));
        } else if (off >= 0xe200 && off < 0xe208) {
            c->nvic_pend[(off - 0xe200) >> 2] |= v & nvic_view(c, ns, (off - 0xe200) >> 2);
            c->check_exc = true;
        } else if (off >= 0xe280 && off < 0xe288) {
            c->nvic_pend[(off - 0xe280) >> 2] &= ~(v & nvic_view(c, ns, (off - 0xe280) >> 2));
        } else if (off >= 0xe380 && off < 0xe388) {
            if (c->v8 && !ns) {
                c->nvic_itns[(off - 0xe380) >> 2] = v;
                c->check_exc = true;
            }
        } else if (off >= 0xe400 && off < 0xe440) {
            unsigned i = off - 0xe400;
            for (unsigned k = 0; k < 4; k++)
                if (!ns || !exc_targets_secure(c, 16 + i + k, true))
                    c->nvic_pri[i + k] = (uint8_t)((v >> (8 * k)) & c->pri_mask);
            c->check_exc = true;
        }
        break;
    }
}

// off is the address less 0xe0000000.
uint32_t cpu_ppb_read(cpu_t *c, uint32_t off)
{
    bool ns = c->ns;
    if (off >= 0x10000) {
        if (!c->v8 || c->ns || (off & 0xff000) != 0x2e000)
            return 0;
        ns = true;
        off &= 0xffff;
    }
    return scs_read(c, off, ns);
}

void cpu_ppb_write(cpu_t *c, uint32_t off, uint32_t v)
{
    bool ns = c->ns;
    if (off >= 0x10000) {
        if (!c->v8 || c->ns || (off & 0xff000) != 0x2e000)
            return;
        ns = true;
        off &= 0xffff;
    }
    scs_write(c, off, v, ns);
}

// MRS and MSR: SYSm values from 0x88 reach the Non-secure registers from Secure state.
static uint32_t read_special(cpu_t *c, unsigned sysm)
{
    bool secure = !c->ns;
    if (sysm >= 0x80 && (!secure || !c->v8))
        return 0;
    switch (sysm) {
    case 0: return c->apsr & 0xf80f0000u;
    case 1:
    case 3: return (c->apsr & 0xf80f0000u) | c->ipsr;
    case 2: return c->apsr & 0xf80f0000u;
    case 5:
    case 7: return c->ipsr;
    case 6: return 0;
    case 8: return sp_get(c, secure, false);
    case 9: return sp_get(c, secure, true);
    case 10: return secure ? c->msplim : c->msplim_ns;
    case 11: return secure ? c->psplim : c->psplim_ns;
    case 16: return (secure ? c->primask : c->primask_ns) & 1;
    case 17:
    case 18: return secure ? c->basepri : c->basepri_ns;
    case 19: return (secure ? c->faultmask : c->faultmask_ns) & 1;
    case 20: return secure ? c->control & 0xf : (c->control_ns & 3) | (c->control & 4);
    case 0x88: return sp_get(c, false, false);
    case 0x89: return sp_get(c, false, true);
    case 0x8a: return c->msplim_ns;
    case 0x8b: return c->psplim_ns;
    case 0x90: return c->primask_ns & 1;
    case 0x91: return c->basepri_ns;
    case 0x93: return c->faultmask_ns & 1;
    case 0x94: return (c->control_ns & 3) | (c->control & 4);
    case 0x98: return sp_get(c, false, c->ipsr == 0 && (c->control_ns & 2));
    default: return 0;
    }
}

static void write_special(cpu_t *c, unsigned sysm, uint32_t v, unsigned mask)
{
    bool secure = !c->ns;
    if (sysm >= 0x80 && (!secure || !c->v8))
        return;
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
    case 8: sp_set(c, secure, false, v & ~3u); break;
    case 9: sp_set(c, secure, true, v & ~3u); break;
    case 10: *(secure ? &c->msplim : &c->msplim_ns) = v & ~7u; break;
    case 11: *(secure ? &c->psplim : &c->psplim_ns) = v & ~7u; break;
    case 16:
    case 0x90:
        *(secure && sysm == 16 ? &c->primask : &c->primask_ns) = v & 1;
        c->check_exc = true;
        break;
    case 17:
    case 0x91:
        *(secure && sysm == 17 ? &c->basepri : &c->basepri_ns) = v & c->pri_mask;
        c->check_exc = true;
        break;
    case 18: {
        uint32_t *bp = secure ? &c->basepri : &c->basepri_ns;
        v &= c->pri_mask;
        if (v && (!*bp || v < *bp))
            *bp = v;
        c->check_exc = true;
        break;
    }
    case 19:
    case 0x93:
        if (exec_priority(c, true) > -1 || !(v & 1))
            *(secure && sysm == 19 ? &c->faultmask : &c->faultmask_ns) = v & 1;
        c->check_exc = true;
        break;
    case 20:
        if (c->ipsr == 0) {
            save_sp(c);
            *control_reg(c, secure) = (*control_reg(c, secure) & ~3u) | (v & 3);
            load_sp(c);
        } else {
            *control_reg(c, secure) = (*control_reg(c, secure) & ~1u) | (v & 1);
        }
        if (secure || (c->nsacr & (1u << 10)))
            c->control = (c->control & ~4u) | (v & 4);
        if (secure && c->v8)
            c->control = (c->control & ~CONTROL_SFPA) | (v & CONTROL_SFPA);
        break;
    case 0x88: sp_set(c, false, false, v & ~3u); break;
    case 0x89: sp_set(c, false, true, v & ~3u); break;
    case 0x8a: c->msplim_ns = v & ~7u; break;
    case 0x8b: c->psplim_ns = v & ~7u; break;
    case 0x94:
        c->control_ns = (c->control_ns & ~3u) | (v & 3);
        c->control = (c->control & ~4u) | (v & 4);
        break;
    case 0x98: sp_set(c, false, c->ipsr == 0 && (c->control_ns & 2), v & ~3u); break;
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
                if (op & 4) {
                    branch_nonsecure(c, pc, op, vm);
                    break;
                }
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
                    *(c->ns ? &c->primask_ns : &c->primask) = disable;
                if (op & 1)
                    *(c->ns ? &c->faultmask_ns : &c->faultmask) = disable;
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
            if (pc < ROM_SIZE && !c->m->rp2040 && !c->m->real_rom) {
                rom_hle(c, op & 0xff);
            } else {
                undefined(c, pc, op);
            }
        } else if (cond == 0xf) {
            if (exc_priority(c, EXC_SVC, !c->ns, true) >= exec_priority(c, true))
                take_fault(c, EXC_HARDFAULT, pc, "SVC at too high a priority");
            else
                exception_entry(c, EXC_SVC, !c->ns, pc + 2);
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
    if (unlikely(pc - c->fw_lo >= c->fw_size) && fetch_check(c, pc))
        return;
    uint16_t op = fetch16(c, pc);
    bool wide = (op >> 11) >= 0x1d;
    if (unlikely(wide && pc + 2 - c->fw_lo >= c->fw_size) && fetch_check_hw2(c, pc))
        return;
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
        take_flagged_fault(c, pc, it);
        return;
    }
    if (it && c->itstate == it)
        it_advance(c);
}

#include "block.inc"
#include "jit.inc"

void cpu_flush_code(void)
{
    memset(blocks, 0, sizeof(blocks));
    memset(block_mru, 0, sizeof(block_mru));
    jit_forget();
}

void cpu_run(cpu_t *c, uint64_t until)
{
    chip_t *m = c->m;
    bool use_blocks = blocks_on();
    bool use_jit = use_blocks && jit_available();
    // Set when native code stopped short of the limit for block_run to reach it exactly. A just
    // taken exception leaves check_exc set, which block_run answers after one instruction.
    bool careful = false;
    if (c->state == CPU_WFI || c->state == CPU_WFE)
        sleep_check(c);
    while (c->t < until) {
        if (unlikely(c->check_exc)) {
            if (unlikely(m->code_flush)) {
                m->code_flush = false;
                cpu_flush_code();
            }
            check_exceptions(c);
            if (c->state != CPU_RUN)
                sleep_check(c);
        }
        if (unlikely(c->state != CPU_RUN))
            return;
        if (likely(use_blocks && !c->itstate && !c->ns)) {
            block_t *b = block_find(m, c->r[15]);
            if (likely(b)) {
                if (use_jit && !careful && !c->check_exc) {
                    careful = jit_run(c, b, until);
                } else {
                    block_run(c, b, until);
                    careful = false;
                }
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
    case CPU_STALL: return "stalled by the RCP";
    default: return "locked up";
    }
}

void cpu_dump(const cpu_t *c)
{
    emu_log("core %d %s%s pc=%08x lr=%08x sp=%08x psr=%08x ipsr=%u primask=%u", c->id, cpu_state_name(c),
            c->ns ? " in Non-secure state" : "", c->r[15], c->r[14], c->r[13], c->apsr, c->ipsr,
            c->ns ? c->primask_ns : c->primask);
    emu_log("  r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x", c->r[0], c->r[1], c->r[2], c->r[3],
            c->r[4], c->r[5], c->r[6], c->r[7]);
    emu_log("  r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x cfsr=%08x hfsr=%08x bfar=%08x", c->r[8], c->r[9], c->r[10],
            c->r[11], c->r[12], c->cfsr, c->hfsr, c->bfar);
}
