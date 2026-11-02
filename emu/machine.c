#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "chip.h"
#include "pcsim/devices.h"
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

void pcsim_machine_lock(void)
{
    pthread_mutex_lock(&lock);
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
    if (!pcsim_opts.flash_path || !chip.flash_dirty)
        return;
    FILE *f = fopen(pcsim_opts.flash_path, "r+b");
    if (!f)
        f = fopen(pcsim_opts.flash_path, "w+b");
    if (!f) {
        pcsim_log("cannot write flash image %s: %s", pcsim_opts.flash_path, strerror(errno));
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
    if (!pcsim_opts.flash_path || pcsim_opts.erase_flash)
        return;
    FILE *f = fopen(pcsim_opts.flash_path, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0 && size <= (long)chip.flash_size && fread(chip.flash, 1, (size_t)size, f) != (size_t)size)
        pcsim_log("short read from flash image %s", pcsim_opts.flash_path);
    fclose(f);
}

void pcsim_flush_devices(void)
{
    pcsim_machine_lock();
    flash_save();
    pcsim_machine_unlock();
    if (pcsim_picocalc.sd)
        sdcard_flush(pcsim_picocalc.sd);
}

// ---------------------------------------------------------------------------------------------
// UF2 download

static bool program_uf2_locked(const char *path, char *err, size_t errlen)
{
    uf2_file_t f;
    if (!uf2_load(path, &f, err, errlen))
        return false;
    bool ok = uf2_program(&chip, &f, err, errlen);
    uf2_free(&f);
    if (ok) {
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
    while (m->now < end && !atomic_load(&stop_flag)) {
        uint64_t target = m->now + CHUNK_PS;
        if (m->events && m->events->when < target)
            target = m->events->when > m->now ? m->events->when : m->now + 1;
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
        pthread_mutex_lock(&m->power_lock);
        bool req = m->req_reset || m->req_bootsel || m->req_poweroff;
        pthread_mutex_unlock(&m->power_lock);
        if (req)
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

static const board_info_t *pick_board(const uf2_file_t *f)
{
    if (pcsim_opts.board) {
        const board_info_t *b = board_find(pcsim_opts.board);
        if (!b)
            pcsim_log("unknown board %s", pcsim_opts.board);
        return b;
    }
    const char *hint = f ? uf2_board_hint(f) : NULL;
    if (hint) {
        const board_info_t *b = board_find(hint);
        if (b)
            return b;
        pcsim_log("image was built for board %s; running it on a pico2", hint);
    }
    return board_find("pico2");
}

bool pcsim_machine_start(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lock, &attr);
    pthread_mutexattr_destroy(&attr);

    uf2_file_t f;
    bool have_uf2 = false;
    char err[256];
    if (pcsim_opts.uf2_path) {
        if (!uf2_load(pcsim_opts.uf2_path, &f, err, sizeof(err))) {
            pcsim_log("%s", err);
            return false;
        }
        have_uf2 = true;
    }
    const board_info_t *b = pick_board(have_uf2 ? &f : NULL);
    if (!b)
        return false;
    chip_init(&chip, b);
    chip.trace = pcsim_opts.trace;
    g_fast_bus = pcsim_opts.fast;
    if (!pcsim_opts.flash_path) {
        static char path[4096];
        const char *home = getenv("HOME");
        const char *xdg = getenv("XDG_DATA_HOME");
        char dir[3600];
        if (xdg && *xdg)
            snprintf(dir, sizeof(dir), "%s/pcsim", xdg);
        else
            snprintf(dir, sizeof(dir), "%s/.local/share/pcsim", home ? home : ".");
        char parent[3600];
        snprintf(parent, sizeof(parent), "%s", dir);
        char *slash = strrchr(parent, '/');
        if (slash) {
            *slash = 0;
            mkdir(parent, 0755);
        }
        mkdir(dir, 0755);
        snprintf(path, sizeof(path), "%s/%s.flash", dir, b->name);
        pcsim_opts.flash_path = path;
    } else if (!strcmp(pcsim_opts.flash_path, "none")) {
        pcsim_opts.flash_path = NULL;
    }
    flash_load();
    picocalc_board_init();
    if (have_uf2) {
        if (!uf2_program(&chip, &f, err, sizeof(err))) {
            pcsim_log("%s", err);
            uf2_free(&f);
            return false;
        }
        uf2_free(&f);
        pcsim_log("wrote %s to %s flash", pcsim_opts.uf2_path, b->name);
        flash_save();
    }
    pcsim_log("%s: RP2350, %u MB flash%s%s", b->name, b->flash_size >> 20, b->psram_size ? ", 8 MB PSRAM" : "",
              b->wifi ? ", CYW43439 (not emulated yet)" : "");
    if (pcsim_opts.flash_path)
        pcsim_log("flash image: %s", pcsim_opts.flash_path);
    uint32_t boot_type = chip.boot_type;
    uint32_t p0 = chip.boot_param[0], p1 = chip.boot_param[1];
    chip_power_on(&chip);
    chip.boot_type = boot_type;
    chip.boot_param[0] = p0;
    chip.boot_param[1] = p1;
    if (pthread_create(&thread, NULL, machine_main, NULL) != 0) {
        pcsim_log("cannot start the emulation thread");
        return false;
    }
    return true;
}

void pcsim_machine_stop(void)
{
    atomic_store(&stop_flag, true);
    pthread_join(thread, NULL);
    if (chip.trace) {
        pcsim_log("stopped at %.3f s emulated, clk_sys %llu Hz", (double)chip.now / PS_PER_S,
                  (unsigned long long)chip.sys_hz);
        for (int i = 0; i < 2; i++) {
            pcsim_log("core %d: %llu instructions", i, (unsigned long long)chip.core[i].insns);
            cpu_dump(&chip.core[i]);
        }
    }
}
