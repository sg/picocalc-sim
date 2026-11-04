#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "chip.h"
#include "pcsim/devices.h"
#include "pcsim/net.h"
#include "pcsim/sim.h"
#include "uf2.h"

// The emulation thread. It runs both cores and the peripheral events in short chunks of emulated
// time, paces them against the host clock so the device runs at its real speed, and handles the
// resets, BOOTSEL and power changes that firmware, devices and the user request.

#define CHUNK_PS (1 * PS_PER_US)
#define SLICE_PS (2 * PS_PER_MS)

static chip_t chip;
static pthread_t thread;
static pthread_mutex_t lock;
static atomic_bool stop_flag;
static _Atomic pcsim_state_t state = PCSIM_RUNNING;
static char state_reason[128];
static atomic_bool power_button, bootsel_held;
static double speed = 1.0;

static pthread_mutex_t uf2_lock = PTHREAD_MUTEX_INITIALIZER;
static char pending_uf2[4096];
// PCSIM_STOP_AT_MS ends the run at an exact emulated time, so that with --trace two builds can be
// compared by the state digest they print.
static uint64_t stop_at_ps;
// PCSIM_RESTART_AT_MS restarts the machine once at an exact emulated time, so the digest the
// second run ends with can be checked against a fresh launch's.
static uint64_t restart_at_ps;

// What pcsim_machine_start has set up, for pcsim_machine_stop or a failed start to take down.
static bool chip_ready, net_up, running;
static atomic_uint starts;
static atomic_bool restart_flag;
// The flash image this run keeps, once "none" and the default are worked out from pcsim_opts.
static char flash_file[4096];
static const char *flash_path;
// With --flash none, each board's flash lasts from one restart to the next, as a power cycle
// would leave it, until pcsim exits.
static uint8_t *kept_flash[8];

// Threads waiting for the lock, which the emulation thread hands over between slices. Running
// behind real time it never sleeps, and would otherwise take the lock back before a waiter woke.
static atomic_int lock_waiters;

void pcsim_machine_lock(void)
{
    atomic_fetch_add(&lock_waiters, 1);
    pthread_mutex_lock(&lock);
    atomic_fetch_sub(&lock_waiters, 1);
}

void pcsim_machine_unlock(void)
{
    pthread_mutex_unlock(&lock);
}

uint64_t pcsim_uptime_us(void)
{
    return chip.now / PS_PER_US;
}

pcsim_state_t pcsim_state(void)
{
    return atomic_load(&state);
}

const char *pcsim_state_reason(void)
{
    return state_reason;
}

const char *pcsim_board_name(void)
{
    return chip.board ? chip.board->name : "?";
}

bool pcsim_board_desc(int i, pcsim_board_desc_t *d)
{
    for (int n = 0; n <= i; n++)
        if (!board_table[n].name)
            return false;
    const board_info_t *b = &board_table[i];
    *d = (pcsim_board_desc_t){b->name, b->rp2040, b->wifi, b->flash_size >> 20, b->psram_size >> 20};
    return true;
}

double pcsim_speed(void)
{
    return speed;
}

static void set_state(pcsim_state_t s, const char *reason)
{
    snprintf(state_reason, sizeof(state_reason), "%s", reason);
    atomic_store(&state, s);
}

void pcsim_reboot(const char *reason)
{
    chip_request(&chip, REQ_RESET, reason);
}

void pcsim_stop(pcsim_state_t s, const char *reason)
{
    if (s == PCSIM_POWERED_OFF)
        chip_request(&chip, REQ_POWEROFF, reason);
    else if (s == PCSIM_BOOTSEL)
        chip_request(&chip, REQ_BOOTSEL, reason);
}

void pcsim_power_button(void)
{
    atomic_store(&power_button, true);
}

void pcsim_bootsel_button(bool held)
{
    atomic_store(&bootsel_held, held);
}

// ---------------------------------------------------------------------------------------------
// Flash persistence

static void flash_save(void)
{
    if (!flash_path || !chip.flash || !chip.flash_dirty)
        return;
    FILE *f = fopen(flash_path, "r+b");
    if (!f)
        f = fopen(flash_path, "w+b");
    if (!f) {
        pcsim_log("cannot write flash image %s: %s", flash_path, strerror(errno));
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size != (long)chip.flash_size) {
        fseek(f, 0, SEEK_SET);
        fwrite(chip.flash, 1, chip.flash_size, f);
    } else {
        fseek(f, (long)chip.flash_dirty_lo, SEEK_SET);
        fwrite(chip.flash + chip.flash_dirty_lo, 1, chip.flash_dirty_hi - chip.flash_dirty_lo, f);
    }
    fclose(f);
    chip.flash_dirty = false;
}

static void flash_load(void)
{
    uint8_t **kept = &kept_flash[chip.board - board_table];
    if (pcsim_opts.erase_flash) {
        free(*kept);
        *kept = NULL;
        return;
    }
    if (!flash_path) {
        if (*kept) {
            free(chip.flash);
            chip.flash = *kept;
            *kept = NULL;
        }
        return;
    }
    FILE *f = fopen(flash_path, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0 && size <= (long)chip.flash_size && fread(chip.flash, 1, (size_t)size, f) != (size_t)size)
        pcsim_log("short read from flash image %s", flash_path);
    fclose(f);
}

// ---------------------------------------------------------------------------------------------
// UF2 download

static bool program_uf2_locked(const char *path, char *err, size_t errlen)
{
    uf2_file_t f;
    if (!uf2_load(path, &f, err, errlen))
        return false;
    bool ok = uf2_program(&chip, &f, err, errlen);
    bool ram = f.ram;
    uf2_free(&f);
    if (ok && ram) {
        pcsim_log("loaded %s into %s RAM", path, chip.board->name);
    } else if (ok) {
        flash_save();
        pcsim_log("wrote %s to %s flash", path, chip.board->name);
    }
    return ok;
}

bool pcsim_flash_uf2(const char *path, char *err, size_t errlen)
{
    uf2_file_t f;
    if (!uf2_load(path, &f, err, errlen))
        return false;
    uf2_free(&f);
    pthread_mutex_lock(&uf2_lock);
    snprintf(pending_uf2, sizeof(pending_uf2), "%s", path);
    pthread_mutex_unlock(&uf2_lock);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Run loop

static void handle_requests(void)
{
    pthread_mutex_lock(&chip.power_lock);
    bool reset = chip.req_reset, bootsel = chip.req_bootsel, poweroff = chip.req_poweroff;
    char reason[96];
    memcpy(reason, chip.req_reason, sizeof(reason));
    chip.req_reset = chip.req_bootsel = chip.req_poweroff = false;
    atomic_store(&chip.req_any, false);
    pthread_mutex_unlock(&chip.power_lock);

    char path[4096] = "";
    pthread_mutex_lock(&uf2_lock);
    if (pending_uf2[0]) {
        memcpy(path, pending_uf2, sizeof(path));
        pending_uf2[0] = 0;
    }
    pthread_mutex_unlock(&uf2_lock);
    if (path[0]) {
        char err[256];
        if (program_uf2_locked(path, err, sizeof(err))) {
            set_state(PCSIM_RUNNING, "");
            chip_reset(&chip, true);
        } else {
            pcsim_log("%s", err);
        }
        return;
    }

    if (poweroff) {
        pcsim_log("power off: %s", reason);
        flash_save();
        set_state(PCSIM_POWERED_OFF, reason);
        for (int i = 0; i < 2; i++)
            chip.core[i].state = CPU_OFF;
        return;
    }
    if (bootsel) {
        pcsim_log("BOOTSEL: %s", reason);
        flash_save();
        set_state(PCSIM_BOOTSEL, reason);
        for (int i = 0; i < 2; i++)
            chip.core[i].state = CPU_OFF;
        return;
    }
    if (reset) {
        if (chip.trace)
            pcsim_log("reset: %s", reason);
        flash_save();
        if (atomic_load(&bootsel_held))
            chip.boot_type = 2;
        set_state(PCSIM_RUNNING, "");
        chip_reset(&chip, true);
    }
    if (atomic_exchange(&power_button, false)) {
        pcsim_state_t s = atomic_load(&state);
        if (s == PCSIM_POWERED_OFF) {
            set_state(PCSIM_RUNNING, "");
            chip.boot_type = 0;
            chip_power_on(&chip);
        }
    }
}

static void run_chunk(uint64_t target)
{
    chip_t *m = &chip;
    for (int i = 0; i < 2; i++) {
        cpu_t *c = &m->core[i];
        if (c->state == CPU_OFF || c->state == CPU_LOCKUP) {
            c->t = target;
            continue;
        }
        if (c->t < target)
            cpu_run(c, target);
        if (c->state != CPU_RUN && c->t < target)
            c->t = target;
        if (c->t > m->now)
            m->now = c->t;
    }
    if (m->now < target)
        m->now = target;
}

static bool cores_idle(void)
{
    for (int i = 0; i < 2; i++)
        if (chip.core[i].state == CPU_RUN)
            return false;
    return true;
}

static void fire_events(void)
{
    chip_t *m = &chip;
    while (m->events && m->events->when <= m->now) {
        emu_event_t *ev = m->events;
        m->events = ev->next;
        ev->queued = false;
        ev->next = NULL;
        ev->fn(m, ev);
    }
}

static void run_slice(void)
{
    chip_t *m = &chip;
    uint64_t end = m->now + SLICE_PS;
    if (stop_at_ps && end > stop_at_ps)
        end = stop_at_ps;
    while (m->now < end && !atomic_load(&stop_flag)) {
        uint64_t target = m->now + CHUNK_PS;
        if (m->events && m->events->when < target)
            target = m->events->when > m->now ? m->events->when : m->now + 1;
        if (stop_at_ps && target > end)
            target = end;
        run_chunk(target);
        fire_events();
        if (cores_idle()) {
            // Nothing runs until the next event, so emulated time can jump straight to it.
            uint64_t next = m->events ? m->events->when : end;
            if (next > end)
                next = end;
            if (next > m->now) {
                m->now = next;
                for (int i = 0; i < 2; i++)
                    if (m->core[i].t < next)
                        m->core[i].t = next;
            }
            fire_events();
            for (int i = 0; i < 2; i++)
                if (m->core[i].state == CPU_WFI || m->core[i].state == CPU_WFE)
                    cpu_poke(&m->core[i]);
            for (int i = 0; i < 2; i++) {
                cpu_t *c = &m->core[i];
                if ((c->state == CPU_WFI || c->state == CPU_WFE) && cpu_irq_wakes(c))
                    break;
            }
        }
        if (atomic_load(&m->req_any))
            break;
    }
}

static void *machine_main(void *arg)
{
    (void)arg;
    uint64_t host0 = pcsim_mono_ns(), emu0 = chip.now;
    uint64_t last_save = host0, speed_host = host0, speed_emu = chip.now;
    pthread_mutex_lock(&lock);
    while (!atomic_load(&stop_flag)) {
        handle_requests();
        pcsim_state_t s = atomic_load(&state);
        if (s != PCSIM_RUNNING) {
            pthread_mutex_unlock(&lock);
            pcsim_sleep_ns(10 * 1000000ull);
            pthread_mutex_lock(&lock);
            host0 = pcsim_mono_ns();
            emu0 = chip.now;
            continue;
        }
        run_slice();
        pinwave_advance(&chip);
        if (restart_at_ps && chip.now >= restart_at_ps) {
            restart_at_ps = 0;
            pcsim_request_restart();
        }
        if (stop_at_ps && chip.now >= stop_at_ps) {
            atomic_store(&stop_flag, true);
            pcsim_request_quit(0);
            break;
        }
        pthread_mutex_unlock(&lock);

        uint64_t host = pcsim_mono_ns();
        if (!pcsim_opts.unthrottled) {
            uint64_t want = host0 + (chip.now - emu0) / PS_PER_NS;
            if (want > host + 200000)
                pcsim_sleep_ns(want - host);
            else if (host > want + 100000000ull) {
                host0 = host;
                emu0 = chip.now;
            }
        }
        if (host - speed_host > 1000000000ull) {
            speed = (double)(chip.now - speed_emu) / PS_PER_NS / (double)(host - speed_host);
            speed_host = host;
            speed_emu = chip.now;
        }
        for (int spins = 0; atomic_load(&lock_waiters) && spins < 1000; spins++)
            sched_yield();
        pthread_mutex_lock(&lock);
        if (host - last_save > 2000000000ull) {
            flash_save();
            last_save = host;
        }
    }
    flash_save();
    pthread_mutex_unlock(&lock);
    return NULL;
}

// The RP2040's boot ROM can't be shipped with the emulator, so it comes from a file: --rom, or
// rp2040-rom.elf (or .bin) in the data directory.
static bool load_rp2040_rom(void)
{
    char path[4096], err[512];
    const char *p = pcsim_opts.rom_path;
    if (!p) {
        pcsim_data_path(path, sizeof(path), "rp2040-rom.elf", false);
        FILE *fp = fopen(path, "rb");
        if (!fp)
            pcsim_data_path(path, sizeof(path), "rp2040-rom.bin", false);
        else
            fclose(fp);
        p = path;
    }
    if (rp2040_load_rom(&chip, p, err, sizeof(err)))
        return true;
    pcsim_log("%s", err);
    pcsim_log("the RP2040 boards run the chip's own boot ROM: download b2.elf from "
              "https://github.com/raspberrypi/pico-bootrom-rp2040/releases and pass it with --rom, or save it as %s",
              pcsim_opts.rom_path ? "rp2040-rom.elf in the data directory" : p);
    return false;
}

static const board_info_t *pick_board(const uf2_file_t *f)
{
    if (pcsim_opts.board) {
        const board_info_t *b = board_find(pcsim_opts.board);
        if (!b)
            pcsim_log("unknown board %s", pcsim_opts.board);
        return b;
    }
    const char *hint = f ? uf2_board_hint(f) : NULL;
    bool rp2040 = f && f->family == UF2_FAMILY_RP2040;
    if (hint) {
        const board_info_t *b = board_find(hint);
        if (b && b->rp2040 == rp2040)
            return b;
    }
    const board_info_t *b = pcsim_opts.default_board ? board_find(pcsim_opts.default_board) : NULL;
    if (!b || (f && b->rp2040 != rp2040))
        b = board_find(rp2040 ? "pico" : "pico2");
    if (hint)
        pcsim_log("image was built for board %s; running it on a %s", hint, b->name);
    return b;
}

// Takes down whatever pcsim_machine_start set up, in the order nothing freed is used again.
static void teardown(void)
{
    // The frontend's other threads look at pcsim_picocalc with the machine locked.
    pcsim_machine_lock();
    picocalc_board_free();
    pcsim_machine_unlock();
    if (chip_ready)
        cyw43_free(&chip);
    devices_detach_all();
    if (net_up)
        pcsim_net_shutdown();
    net_up = false;
    // Also after a failed start, which would otherwise lose what flash_load took out.
    if (chip_ready && !flash_path) {
        uint8_t **kept = &kept_flash[chip.board - board_table];
        free(*kept);
        *kept = chip.flash;
        chip.flash = NULL;
    }
    if (chip_ready)
        chip_free(&chip);
    chip_ready = false;
    pthread_mutex_lock(&uf2_lock);
    pending_uf2[0] = 0;
    pthread_mutex_unlock(&uf2_lock);
}

static bool start(uf2_file_t *f)
{
    const char *stop_ms = getenv("PCSIM_STOP_AT_MS");
    stop_at_ps = stop_ms && *stop_ms ? (uint64_t)(atof(stop_ms) * PS_PER_MS) : 0;
    static bool restart_at_read;
    if (!restart_at_read) {
        restart_at_read = true;
        const char *restart_ms = getenv("PCSIM_RESTART_AT_MS");
        if (restart_ms && *restart_ms)
            restart_at_ps = (uint64_t)(atof(restart_ms) * PS_PER_MS);
    }
    atomic_store(&stop_flag, false);
    atomic_store(&power_button, false);
    set_state(PCSIM_RUNNING, "");
    speed = 1.0;

    char err[256];
    if (pcsim_opts.uf2_path && !uf2_load(pcsim_opts.uf2_path, f, err, sizeof(err))) {
        pcsim_log("%s", err);
        return false;
    }
    bool have_uf2 = f->blocks != NULL;
    const board_info_t *b = pick_board(have_uf2 ? f : NULL);
    if (!b)
        return false;
    chip_init(&chip, b);
    chip_ready = true;
    chip.trace = pcsim_opts.trace;
    g_fast_bus = pcsim_opts.fast;
    if (!pcsim_opts.flash_path) {
        char file[64];
        snprintf(file, sizeof(file), "%s.flash", b->name);
        pcsim_data_path(flash_file, sizeof(flash_file), file, true);
        flash_path = flash_file;
    } else if (!strcmp(pcsim_opts.flash_path, "none")) {
        flash_path = NULL;
    } else {
        snprintf(flash_file, sizeof(flash_file), "%s", pcsim_opts.flash_path);
        flash_path = flash_file;
    }
    flash_load();
    if (b->rp2040 && !load_rp2040_rom())
        return false;
    pcsim_machine_lock();
    bool board = picocalc_board_init();
    pcsim_machine_unlock();
    if (!board)
        return false;
    // The card is in the slot when the PicoCalc is switched on, as a fresh launch has it.
    if (pcsim_opts.card_path && !sdcard_insert(pcsim_picocalc.sd, pcsim_opts.card_path))
        pcsim_log("continuing with the card slot empty");
    if (b->wifi && !pcsim_opts.no_wifi) {
        net_up = true;
        if (!pcsim_net_init())
            return false;
        cyw43_init(&chip);
    }
    if (have_uf2 && !f->ram) {
        if (!uf2_program(&chip, f, err, sizeof(err))) {
            pcsim_log("%s", err);
            return false;
        }
        pcsim_log("wrote %s to %s flash", pcsim_opts.uf2_path, b->name);
        flash_save();
    }
    pcsim_log("%s: %s, %u MB flash%s%s", b->name, b->rp2040 ? "RP2040" : "RP2350", b->flash_size >> 20,
              b->psram_size ? ", 8 MB PSRAM" : "",
              !b->wifi ? "" : pcsim_opts.no_wifi ? ", CYW43439 left out (--no-wifi)" : ", CYW43439 WiFi");
    if (flash_path)
        pcsim_log("flash image: %s", flash_path);
    uint32_t boot_type = chip.boot_type;
    uint32_t p0 = chip.boot_param[0], p1 = chip.boot_param[1];
    chip_power_on(&chip);
    chip.boot_type = boot_type;
    chip.boot_param[0] = p0;
    chip.boot_param[1] = p1;
    // A RAM image goes in once power-on has given SRAM its random contents, as BOOTSEL's drive
    // would put it there, and the board then resets without losing it.
    if (have_uf2 && f->ram) {
        if (!uf2_program(&chip, f, err, sizeof(err))) {
            pcsim_log("%s", err);
            return false;
        }
        pcsim_log("loaded %s into %s RAM", pcsim_opts.uf2_path, b->name);
        chip_reset(&chip, true);
    }
    if (pthread_create(&thread, NULL, machine_main, NULL) != 0) {
        pcsim_log("cannot start the emulation thread");
        return false;
    }
    return true;
}

bool pcsim_machine_running(void)
{
    return running;
}

bool pcsim_machine_start(void)
{
    if (running)
        return true;
    static bool lock_ready;
    if (!lock_ready) {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&lock, &attr);
        pthread_mutexattr_destroy(&attr);
        lock_ready = true;
    }
    uf2_file_t f = {0};
    bool ok = start(&f);
    uf2_free(&f);
    if (!ok) {
        teardown();
        return false;
    }
    running = true;
    atomic_fetch_add(&starts, 1);
    return true;
}

static uint64_t fnv1a(uint64_t h, const void *data, size_t len)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// Everything a change to the CPU core could disturb: registers, time and instruction counts on
// both cores, and the whole of SRAM.
static uint64_t state_digest(void)
{
    uint64_t h = 0xcbf29ce484222325ull;
    h = fnv1a(h, &chip.now, sizeof(chip.now));
    for (int i = 0; i < 2; i++) {
        const cpu_t *c = &chip.core[i];
        h = fnv1a(h, c->r, sizeof(c->r));
        h = fnv1a(h, &c->apsr, sizeof(c->apsr));
        h = fnv1a(h, &c->ipsr, sizeof(c->ipsr));
        h = fnv1a(h, &c->itstate, sizeof(c->itstate));
        h = fnv1a(h, &c->state, sizeof(c->state));
        h = fnv1a(h, &c->t, sizeof(c->t));
        h = fnv1a(h, &c->insns, sizeof(c->insns));
        h = fnv1a(h, c->s, sizeof(c->s));
        h = fnv1a(h, &c->fpscr, sizeof(c->fpscr));
    }
    return fnv1a(h, chip.sram, sizeof(chip.sram));
}

void pcsim_machine_stop(void)
{
    if (!running)
        return;
    atomic_store(&stop_flag, true);
    pthread_join(thread, NULL);
    running = false;
    if (chip.trace) {
        pcsim_log("stopped at %.3f s emulated, clk_sys %llu Hz", (double)chip.now / PS_PER_S,
                  (unsigned long long)chip.sys_hz);
        for (int i = 0; i < 2; i++) {
            pcsim_log("core %d: %llu instructions", i, (unsigned long long)chip.core[i].insns);
            cpu_dump(&chip.core[i]);
        }
        pcsim_log("state digest %016llx", (unsigned long long)state_digest());
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++) {
                pio_sm_t *sm = &chip.pio[i].sm[j];
                if (chip.pio[i].ctrl & (1u << j))
                    pcsim_log("pio%d sm%d pc=%u x=%08x y=%08x osr=%d isr=%d tx=%d rx=%d%s", i, j, sm->pc, sm->x, sm->y,
                              sm->osr_count, sm->isr_count, sm->txn, sm->rxn, sm->stalled ? " stalled" : "");
            }
        for (int i = 0; i < 16; i++) {
            dma_ch_t *c = &chip.dma.ch[i];
            if (c->ctrl & (1u << 26))
                pcsim_log("dma%d busy ctrl=%08x read=%08x write=%08x count=%08x", i, c->ctrl, c->read_addr,
                          c->write_addr, c->trans_count);
        }
    }
    teardown();
}

bool pcsim_machine_restart(void)
{
    pcsim_machine_stop();
    return pcsim_machine_start();
}

void pcsim_request_restart(void)
{
    atomic_store(&restart_flag, true);
}

bool pcsim_restart_requested(void)
{
    return atomic_exchange(&restart_flag, false);
}

unsigned pcsim_machine_starts(void)
{
    return atomic_load(&starts);
}
