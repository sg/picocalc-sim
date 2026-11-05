#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// MinGW checks formats against whichever printf the build selected, here its C99 one.
#ifndef PICOCALC_SIM_PRINTF_FORMAT
#ifdef __MINGW_PRINTF_FORMAT
#define PICOCALC_SIM_PRINTF_FORMAT __MINGW_PRINTF_FORMAT
#else
#define PICOCALC_SIM_PRINTF_FORMAT printf
#endif
#endif

#if defined(__GNUC__)
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif

typedef struct chip chip_t;
typedef struct cpu cpu_t;

// Emulated time is kept in picoseconds so clock changes never accumulate rounding drift.
#define PS_PER_NS 1000ull
#define PS_PER_US 1000000ull
#define PS_PER_MS 1000000000ull
#define PS_PER_S 1000000000000ull

typedef struct emu_event {
    uint64_t when;
    void (*fn)(chip_t *m, struct emu_event *ev);
    void *ctx;
    struct emu_event *next;
    bool queued;
} emu_event_t;

void ev_schedule(chip_t *m, emu_event_t *ev, uint64_t when);
void ev_cancel(chip_t *m, emu_event_t *ev);

enum {
    CPU_RUN,
    CPU_WFI,
    CPU_WFE,
    CPU_OFF,
    CPU_LOCKUP,
    // Held by the RCP, which stalls the coprocessor port until the core is reset.
    CPU_STALL,
};

#define MAX_EXC 96

typedef struct {
    uint32_t csr, rvr, cvr;
    uint64_t t;
    bool countflag;
    emu_event_t ev;
} systick_t;

// One security state's MPU, which only TT consults: accesses are not checked against it.
typedef struct {
    uint32_t ctrl, rnr, rbar[8], rlar[8], mair[2];
} mpu_t;

// Banked registers have a Secure copy under the plain name and a Non-secure one ending in _ns, and
// r[13] is the current stack pointer, whose own slot is stale until the core switches stacks.
struct cpu {
    uint32_t r[16];
    uint32_t apsr;
    uint32_t ipsr;
    uint32_t itstate;
    uint32_t msp, psp, msplim, psplim;
    uint32_t control;
    uint32_t primask, basepri, faultmask;
    int id;
    int state;
    bool v8;
    bool event;
    bool check_exc;
    bool fault;
    // The exception a fault flagged by a memory access or check raises: a BusFault, UsageFault
    // or SecureFault.
    uint8_t fault_exc;
    bool ns;
    // Whether the core's bus accesses are Non-secure: in Non-secure state, or on core 1 while
    // ACCESSCTRL's FORCE_CORE_NS is set.
    bool bus_ns;
    uint32_t cyc;
    uint32_t fetch_line;
    uint64_t t;
    uint64_t insns;
    // Loads and stores go through these: the chip's pages in Secure state, and none in
    // Non-secure state, so that every access there takes the path that checks its attribution.
    uint8_t **rpage, **wpage;
    // Instruction fetches from fw_lo up to fw_lo + fw_size need no attribution check, nor do
    // Non-secure loads and stores in the same way from dw_lo.
    uint32_t fw_lo, fw_size, dw_lo, dw_size;

    uint32_t msp_ns, psp_ns, msplim_ns, psplim_ns;
    uint32_t control_ns, primask_ns, basepri_ns, faultmask_ns;

    uint32_t nvic_en[2], nvic_pend[2], nvic_itns[2];
    uint8_t nvic_pri[64];
    uint8_t shpr[16], shpr_ns[16];
    bool pend_pendsv, pend_systick, pend_pendsv_ns, pend_systick_ns, pend_nmi;
    // Active exceptions by number, with banked exceptions' Secure instances; active_ns holds
    // their Non-secure instances.
    uint32_t active[3], active_ns;
    uint32_t vtor, scr, ccr, shcsr, cfsr, hfsr, mmfar, bfar, cpacr, nsacr, aircr;
    uint32_t vtor_ns, scr_ns, ccr_ns, shcsr_ns, cfsr_ns, mmfar_ns, cpacr_ns, aircr_ns;
    uint32_t fpccr, fpcar, fpdscr, fpccr_ns, fpcar_ns, fpdscr_ns;
    mpu_t mpu[2];
    uint32_t sau_ctrl, sau_rnr, sau_rbar[8], sau_rlar[8], sfsr, sfar;
    uint8_t pri_mask;

    uint32_t s[32];
    uint32_t fpscr;

    bool excl_valid;
    uint32_t excl_addr;

    // The Secure SysTick, then the Non-secure one.
    systick_t syst[2];

    int hle_state;
    uint32_t hle_val;

    chip_t *m;
};

void cpu_reset(cpu_t *c);
void cpu_run(cpu_t *c, uint64_t until);
// Forgets every translated block, which a new machine could otherwise mistake for its own code.
void cpu_flush_code(void);
uint32_t cpu_ppb_read(cpu_t *c, uint32_t off);
void cpu_ppb_write(cpu_t *c, uint32_t off, uint32_t v);
static inline void cpu_poke(cpu_t *c)
{
    c->check_exc = true;
}
void cpu_wake(cpu_t *c, bool event);
bool cpu_irq_wakes(cpu_t *c);
// Whether Non-secure state may access a; if not, a SecureFault is flagged.
bool cpu_ns_access(cpu_t *c, uint32_t a);
// Whether the SAU and IDAU make a load or store to a Non-secure.
bool cpu_attr_ns(cpu_t *c, uint32_t a);
static inline bool cpu_privileged(const cpu_t *c)
{
    return c->ipsr != 0 || !((c->ns ? c->control_ns : c->control) & 1);
}
void cpu_undefined(cpu_t *c, uint32_t pc, uint32_t op);
const char *cpu_state_name(const cpu_t *c);
void cpu_dump(const cpu_t *c);
void cpu_return_from_hle(cpu_t *c);

void emu_log(const char *fmt, ...) __attribute__((format(PICOCALC_SIM_PRINTF_FORMAT, 1, 2)));
