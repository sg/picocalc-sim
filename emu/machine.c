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
#include "picocalc-sim/devices.h"
#include "picocalc-sim/net.h"
#include "picocalc-sim/sim.h"
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
static _Atomic picocalc_sim_state_t state = PICOCALC_SIM_RUNNING;
static char state_reason[128];
static atomic_bool power_button, bootsel_held, button_reset;
static double speed = 1.0;

static pthread_mutex_t uf2_lock = PTHREAD_MUTEX_INITIALIZER;
static char pending_uf2[4096];
// PICOCALC_SIM_STOP_AT_MS ends the run at an exact emulated time, so that with --trace two builds
// can be compared by the state digest they print.
static uint64_t stop_at_ps;
// PICOCALC_SIM_RESTART_AT_MS restarts the machine once at an exact emulated time, so the digest
// the second run ends with can be checked against a fresh launch's.
static uint64_t restart_at_ps;

// What picocalc_sim_machine_start has set up, for picocalc_sim_machine_stop or a failed start to
// take down.
static bool chip_ready, net_up, running;
static atomic_uint starts;
static atomic_bool restart_flag;
// The flash image this run keeps, once "none" and the default are worked out from
// picocalc_sim_opts.
static char flash_file[4096];
static const char *flash_path;
// With --flash none, each board's flash lasts from one restart to the next, as a power cycle
// would leave it, until picocalc-sim exits.
static uint8_t *kept_flash[8];

// Threads waiting for the lock, which the emulation thread hands over between slices. Running
// behind real time it never sleeps, and would otherwise take the lock back before a waiter woke.
static atomic_int lock_waiters;

void picocalc_sim_machine_lock(void)
{
    atomic_fetch_add(&lock_waiters, 1);
    pthread_mutex_lock(&lock);
    atomic_fetch_sub(&lock_waiters, 1);
}

void picocalc_sim_machine_unlock(void)
{
    pthread_mutex_unlock(&lock);
}

uint64_t picocalc_sim_uptime_us(void)
{
    return chip.now / PS_PER_US;
}

picocalc_sim_state_t picocalc_sim_state(void)
{
    return atomic_load(&state);
}

const char *picocalc_sim_state_reason(void)
{
    return state_reason;
}

const char *picocalc_sim_board_name(void)
{
    return chip.board ? chip.board->name : "?";
}

bool picocalc_sim_board_desc(int i, picocalc_sim_board_desc_t *d)
{
    for (int n = 0; n <= i; n++)
        if (!board_table[n].name)
            return false;
    const board_info_t *b = &board_table[i];
    *d = (picocalc_sim_board_desc_t){b->name, b->rp2040, b->wifi, b->flash_size >> 20, b->psram_size >> 20};
    return true;
}

double picocalc_sim_speed(void)
{
    return speed;
}

static void set_state(picocalc_sim_state_t s, const char *reason)
{
    snprintf(state_reason, sizeof(state_reason), "%s", reason);
    atomic_store(&state, s);
}

void picocalc_sim_reboot(const char *reason)
{
    atomic_store(&button_reset, true);
    chip_request(&chip, REQ_RESET, reason);
}

void picocalc_sim_stop(picocalc_sim_state_t s, const char *reason)
{
    if (s == PICOCALC_SIM_POWERED_OFF)
        chip_request(&chip, REQ_POWEROFF, reason);
    else if (s == PICOCALC_SIM_BOOTSEL)
        chip_request(&chip, REQ_BOOTSEL, reason);
}

void picocalc_sim_power_button(void)
{
    atomic_store(&power_button, true);
}

void picocalc_sim_bootsel_button(bool held)
{
    atomic_store(&bootsel_held, held);
}

void picocalc_sim_usb_plug(bool plugged)
{
    picocalc_sim_machine_lock();
    if (chip_ready && plugged != (chip.usb.host != NULL)) {
        if (plugged)
            usbhost_init(&chip);
        else
            usbhost_free(&chip);
        gpio_refresh_mask(&chip, 1ull << 24);
        picocalc_sim_log("usb: cable %s", plugged ? "plugged into a host" : "unplugged");
    }
    picocalc_sim_machine_unlock();
}

size_t picocalc_sim_usb_serial_send(const uint8_t *data, size_t len)
{
    picocalc_sim_machine_lock();
    size_t n = chip_ready ? usbhost_serial_send(&chip, data, len) : 0;
    picocalc_sim_machine_unlock();
    return n;
}

static atomic_uint serial_terminals;

static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    return b;
}

void picocalc_sim_usb_serial_attach(unsigned terminal, bool on)
{
    picocalc_sim_machine_lock();
    unsigned was = atomic_load(&serial_terminals);
    unsigned now = on ? was | terminal : was & ~terminal;
    atomic_store(&serial_terminals, now);
    if (chip_ready && !was != !now)
        usbhost_serial_wanted_changed(&chip);
    picocalc_sim_machine_unlock();
}

bool picocalc_sim_usb_serial_attached(unsigned terminal)
{
    return (atomic_load(&serial_terminals) & terminal) != 0;
}

bool picocalc_sim_usb_serial_wanted(void)
{
    return atomic_load(&serial_terminals) != 0;
}

int picocalc_sim_usb_serial_state(char *product, size_t cap)
{
    picocalc_sim_machine_lock();
    int s = chip_ready ? usbhost_serial_state(&chip, product, cap) : PICOCALC_SIM_USB_SERIAL_UNPLUGGED;
    if (!chip_ready && cap)
        *product = 0;
    picocalc_sim_machine_unlock();
    return s;
}

bool picocalc_sim_usb_drive_copy(const char *path, char *err, size_t errlen)
{
    size_t len;
    uint8_t *data = uf2_read_raw(path, &len, err, errlen);
    if (!data)
        return false;
    picocalc_sim_machine_lock();
    bool plugged = chip_ready && chip.usb.host;
    bool ok = plugged && usbhost_drive_copy(&chip, data, len, base_name(path));
    picocalc_sim_machine_unlock();
    if (!plugged) {
        free(data);
        snprintf(err, errlen, "the USB cable is unplugged");
    } else if (!ok) {
        snprintf(err, errlen, "the drive is busy copying another UF2");
    }
    return ok;
}

bool picocalc_sim_usb_serial_is_open(void)
{
    picocalc_sim_machine_lock();
    bool open = chip_ready && usbhost_serial_is_open(&chip);
    picocalc_sim_machine_unlock();
    return open;
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
        picocalc_sim_log("cannot write flash image %s: %s", flash_path, strerror(errno));
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
    if (picocalc_sim_opts.erase_flash) {
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
        picocalc_sim_log("short read from flash image %s", flash_path);
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
        picocalc_sim_log("loaded %s into %s RAM", path, chip.board->name);
    } else if (ok) {
        flash_save();
        picocalc_sim_log("wrote %s to %s flash", path, chip.board->name);
    }
    return ok;
}

bool picocalc_sim_flash_uf2(const char *path, char *err, size_t errlen)
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
        // With a boot ROM's drive mounted, the file goes onto the drive and the ROM writes it.
        if (usbhost_drive_mounted(&chip)) {
            size_t len;
            uint8_t *data = uf2_read_raw(path, &len, err, sizeof(err));
            if (!data)
                picocalc_sim_log("%s", err);
            else if (!usbhost_drive_copy(&chip, data, len, base_name(path)))
                picocalc_sim_log("the drive is busy copying another UF2");
            return;
        }
        if (program_uf2_locked(path, err, sizeof(err))) {
            set_state(PICOCALC_SIM_RUNNING, "");
            chip_reset(&chip, true);
        } else {
            picocalc_sim_log("%s", err);
        }
        return;
    }

    if (poweroff) {
        picocalc_sim_log("power off: %s", reason);
        flash_save();
        set_state(PICOCALC_SIM_POWERED_OFF, reason);
        for (int i = 0; i < 2; i++)
            chip.core[i].state = CPU_OFF;
        return;
    }
    if (bootsel) {
        picocalc_sim_log("BOOTSEL: %s", reason);
        flash_save();
        set_state(PICOCALC_SIM_BOOTSEL, reason);
        for (int i = 0; i < 2; i++)
            chip.core[i].state = CPU_OFF;
        return;
    }
    if (reset) {
        if (chip.trace)
            picocalc_sim_log("reset: %s", reason);
        flash_save();
        set_state(PICOCALC_SIM_RUNNING, "");
        bool button = atomic_exchange(&button_reset, false), held = atomic_load(&bootsel_held);
        // The reset button pulls RUN low, which resets the watchdog too, unlike its own reboots.
        if (button) {
            memset(chip.wd_scratch, 0, sizeof(chip.wd_scratch));
            chip.wd_reason = 0;
            chip.bootram_write_once[0] = chip.bootram_write_once[1] = 0;
            chip.boot_type = held ? 2 : 0;
        }
        chip_reset(&chip, true);
        // A person holds BOOTSEL through the reset button and lets go once the board has started,
        // long after the ROM has looked at it, and before any reset the ROM itself then makes.
        if (button)
            chip.bootsel_until = held && chip.real_rom ? chip.now + 100 * PS_PER_MS : 0;
    }
    if (atomic_exchange(&power_button, false)) {
        picocalc_sim_state_t s = atomic_load(&state);
        if (s == PICOCALC_SIM_POWERED_OFF) {
            set_state(PICOCALC_SIM_RUNNING, "");
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
        if (c->state == CPU_OFF || c->state == CPU_LOCKUP || c->state == CPU_STALL) {
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
    uint64_t host0 = picocalc_sim_mono_ns(), emu0 = chip.now;
    uint64_t last_save = host0, speed_host = host0, speed_emu = chip.now;
    pthread_mutex_lock(&lock);
    while (!atomic_load(&stop_flag)) {
        handle_requests();
        picocalc_sim_state_t s = atomic_load(&state);
        if (s != PICOCALC_SIM_RUNNING) {
            pthread_mutex_unlock(&lock);
            picocalc_sim_sleep_ns(10 * 1000000ull);
            pthread_mutex_lock(&lock);
            host0 = picocalc_sim_mono_ns();
            emu0 = chip.now;
            continue;
        }
        run_slice();
        pinwave_advance(&chip);
        if (restart_at_ps && chip.now >= restart_at_ps) {
            restart_at_ps = 0;
            picocalc_sim_request_restart();
        }
        if (stop_at_ps && chip.now >= stop_at_ps) {
            atomic_store(&stop_flag, true);
            picocalc_sim_request_quit(0);
            break;
        }
        pthread_mutex_unlock(&lock);

        uint64_t host = picocalc_sim_mono_ns();
        if (!picocalc_sim_opts.unthrottled) {
            uint64_t want = host0 + (chip.now - emu0) / PS_PER_NS;
            if (want > host + 200000)
                picocalc_sim_sleep_ns(want - host);
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
    const char *p = picocalc_sim_opts.rom_path;
    if (!p) {
        picocalc_sim_data_path(path, sizeof(path), "rp2040-rom.elf", false);
        FILE *fp = fopen(path, "rb");
        if (!fp)
            picocalc_sim_data_path(path, sizeof(path), "rp2040-rom.bin", false);
        else
            fclose(fp);
        p = path;
    }
    if (rp2040_load_rom(&chip, p, err, sizeof(err)))
        return true;
    picocalc_sim_log("%s", err);
    picocalc_sim_log("the RP2040 boards run the chip's own boot ROM: download b2.elf from "
              "https://github.com/raspberrypi/pico-bootrom-rp2040/releases and pass it with --rom, or save it as %s",
              picocalc_sim_opts.rom_path ? "rp2040-rom.elf in the data directory" : p);
    return false;
}

static const board_info_t *pick_board(const uf2_file_t *f)
{
    if (picocalc_sim_opts.board) {
        const board_info_t *b = board_find(picocalc_sim_opts.board);
        if (!b)
            picocalc_sim_log("unknown board %s", picocalc_sim_opts.board);
        return b;
    }
    const char *hint = f ? uf2_board_hint(f) : NULL;
    bool rp2040 = f && f->family == UF2_FAMILY_RP2040;
    if (hint) {
        const board_info_t *b = board_find(hint);
        if (b && b->rp2040 == rp2040)
            return b;
    }
    const board_info_t *b = picocalc_sim_opts.default_board ? board_find(picocalc_sim_opts.default_board) : NULL;
    if (!b || (f && b->rp2040 != rp2040))
        b = board_find(rp2040 ? "pico" : "pico2");
    if (hint)
        picocalc_sim_log("image was built for board %s; running it on a %s", hint, b->name);
    return b;
}

// Takes down whatever picocalc_sim_machine_start set up, in the order nothing freed is used again.
static void teardown(void)
{
    // The frontend's other threads look at picocalc_sim_picocalc with the machine locked.
    picocalc_sim_machine_lock();
    picocalc_board_free();
    picocalc_sim_machine_unlock();
    if (chip_ready) {
        cyw43_free(&chip);
        usbhost_free(&chip);
    }
    devices_detach_all();
    if (net_up)
        picocalc_sim_net_shutdown();
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
    const char *stop_ms = getenv("PICOCALC_SIM_STOP_AT_MS");
    stop_at_ps = stop_ms && *stop_ms ? (uint64_t)(atof(stop_ms) * PS_PER_MS) : 0;
    static bool restart_at_read;
    if (!restart_at_read) {
        restart_at_read = true;
        const char *restart_ms = getenv("PICOCALC_SIM_RESTART_AT_MS");
        if (restart_ms && *restart_ms)
            restart_at_ps = (uint64_t)(atof(restart_ms) * PS_PER_MS);
    }
    atomic_store(&stop_flag, false);
    atomic_store(&power_button, false);
    set_state(PICOCALC_SIM_RUNNING, "");
    speed = 1.0;

    char err[256];
    if (picocalc_sim_opts.uf2_path && !uf2_load(picocalc_sim_opts.uf2_path, f, err, sizeof(err))) {
        picocalc_sim_log("%s", err);
        return false;
    }
    bool have_uf2 = f->blocks != NULL;
    const board_info_t *b = pick_board(have_uf2 ? f : NULL);
    if (!b)
        return false;
    chip_init(&chip, b);
    chip_ready = true;
    if (picocalc_sim_opts.usb)
        usbhost_init(&chip);
    chip.trace = picocalc_sim_opts.trace;
    g_fast_bus = picocalc_sim_opts.fast;
    if (!picocalc_sim_opts.flash_path) {
        char file[64];
        snprintf(file, sizeof(file), "%s.flash", b->name);
        picocalc_sim_data_path(flash_file, sizeof(flash_file), file, true);
        flash_path = flash_file;
    } else if (!strcmp(picocalc_sim_opts.flash_path, "none")) {
        flash_path = NULL;
    } else {
        snprintf(flash_file, sizeof(flash_file), "%s", picocalc_sim_opts.flash_path);
        flash_path = flash_file;
    }
    flash_load();
    if (b->rp2040 && !load_rp2040_rom())
        return false;
    if (!b->rp2040 && picocalc_sim_opts.rp2350_rom_path) {
        char err[512];
        if (!rp2350_load_rom(&chip, picocalc_sim_opts.rp2350_rom_path, err, sizeof(err))) {
            picocalc_sim_log("%s", err);
            return false;
        }
        picocalc_sim_log("boot ROM: %s", picocalc_sim_opts.rp2350_rom_path);
    }
    picocalc_sim_machine_lock();
    bool board = picocalc_board_init();
    picocalc_sim_machine_unlock();
    if (!board)
        return false;
    // The card is in the slot when the PicoCalc is switched on, as a fresh launch has it.
    if (picocalc_sim_opts.card_path && !sdcard_insert(picocalc_sim_picocalc.sd, picocalc_sim_opts.card_path))
        picocalc_sim_log("continuing with the card slot empty");
    if (b->wifi && !picocalc_sim_opts.no_wifi) {
        net_up = true;
        if (!picocalc_sim_net_init())
            return false;
        cyw43_init(&chip);
    }
    if (have_uf2 && !f->ram) {
        if (!uf2_program(&chip, f, err, sizeof(err))) {
            picocalc_sim_log("%s", err);
            return false;
        }
        picocalc_sim_log("wrote %s to %s flash", picocalc_sim_opts.uf2_path, b->name);
        flash_save();
    }
    picocalc_sim_log("%s: %s, %u MB flash%s%s", b->name, b->rp2040 ? "RP2040" : "RP2350", b->flash_size >> 20,
              b->psram_size ? ", 8 MB PSRAM" : "",
              !b->wifi ? "" : picocalc_sim_opts.no_wifi ? ", CYW43439 left out (--no-wifi)" : ", CYW43439 WiFi");
    if (flash_path)
        picocalc_sim_log("flash image: %s", flash_path);
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
            picocalc_sim_log("%s", err);
            return false;
        }
        picocalc_sim_log("loaded %s into %s RAM", picocalc_sim_opts.uf2_path, b->name);
        chip_reset(&chip, true);
    }
    if (pthread_create(&thread, NULL, machine_main, NULL) != 0) {
        picocalc_sim_log("cannot start the emulation thread");
        return false;
    }
    return true;
}

bool picocalc_sim_machine_running(void)
{
    return running;
}

bool picocalc_sim_machine_start(void)
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

void picocalc_sim_machine_stop(void)
{
    if (!running)
        return;
    atomic_store(&stop_flag, true);
    pthread_join(thread, NULL);
    running = false;
    if (chip.trace) {
        picocalc_sim_log("stopped at %.3f s emulated, clk_sys %llu Hz", (double)chip.now / PS_PER_S,
                  (unsigned long long)chip.sys_hz);
        for (int i = 0; i < 2; i++) {
            picocalc_sim_log("core %d: %llu instructions", i, (unsigned long long)chip.core[i].insns);
            cpu_dump(&chip.core[i]);
        }
        picocalc_sim_log("state digest %016llx", (unsigned long long)state_digest());
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++) {
                pio_sm_t *sm = &chip.pio[i].sm[j];
                if (chip.pio[i].ctrl & (1u << j))
                    picocalc_sim_log("pio%d sm%d pc=%u x=%08x y=%08x osr=%d isr=%d tx=%d rx=%d%s", i, j, sm->pc, sm->x, sm->y,
                              sm->osr_count, sm->isr_count, sm->txn, sm->rxn, sm->stalled ? " stalled" : "");
            }
        for (int i = 0; i < 16; i++) {
            dma_ch_t *c = &chip.dma.ch[i];
            if (c->ctrl & (1u << 26))
                picocalc_sim_log("dma%d busy ctrl=%08x read=%08x write=%08x count=%08x", i, c->ctrl, c->read_addr,
                          c->write_addr, c->trans_count);
        }
    }
    teardown();
}

bool picocalc_sim_machine_restart(void)
{
    picocalc_sim_machine_stop();
    return picocalc_sim_machine_start();
}

void picocalc_sim_request_restart(void)
{
    atomic_store(&restart_flag, true);
}

bool picocalc_sim_restart_requested(void)
{
    return atomic_exchange(&restart_flag, false);
}

unsigned picocalc_sim_machine_starts(void)
{
    return atomic_load(&starts);
}
