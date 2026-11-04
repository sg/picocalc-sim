#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// MinGW checks formats against whichever printf the build selected, here its C99 one.
#ifndef PCSIM_PRINTF_FORMAT
#ifdef __MINGW_PRINTF_FORMAT
#define PCSIM_PRINTF_FORMAT __MINGW_PRINTF_FORMAT
#else
#define PCSIM_PRINTF_FORMAT printf
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
};

#define MAX_EXC 96

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
    bool fault_usage;
    uint32_t cyc;
    uint32_t fetch_line;
    uint64_t t;
    uint64_t insns;

    uint32_t nvic_en[2], nvic_pend[2];
    uint8_t nvic_pri[64];
    uint8_t shpr[16];
    bool pend_pendsv, pend_systick, pend_nmi;
    uint32_t active[3];
    uint32_t vtor, scr, ccr, shcsr, cfsr, hfsr, mmfar, bfar, cpacr, nsacr, aircr;
    uint32_t fpccr, fpcar, fpdscr;
    uint8_t pri_mask;

    uint32_t s[32];
    uint32_t fpscr;

    bool excl_valid;
    uint32_t excl_addr;

    uint32_t syst_csr, syst_rvr, syst_cvr;
    uint64_t syst_t;
    bool syst_countflag;
    emu_event_t syst_ev;

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
const char *cpu_state_name(const cpu_t *c);
void cpu_dump(const cpu_t *c);
void cpu_return_from_hle(cpu_t *c);

void emu_log(const char *fmt, ...) __attribute__((format(PCSIM_PRINTF_FORMAT, 1, 2)));
