#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#endif

#include "picocalc-sim/port.h"
#include "picocalc-sim/sim.h"

picocalc_sim_options_t picocalc_sim_opts = {
    .screenshot_dir = ".",
    .scale = 2,
    .battery = 85,
    .volume = 50,
};

static atomic_bool quit_flag;
static atomic_int quit_code;

#define LOG_KEEP 256
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static char log_lines[LOG_KEEP][512];
static unsigned log_total;

void picocalc_sim_log(const char *fmt, ...)
{
    va_list ap, kept;
    va_start(ap, fmt);
    va_copy(kept, ap);
    pthread_mutex_lock(&log_lock);
    fputs("[picocalc-sim] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    vsnprintf(log_lines[log_total++ % LOG_KEEP], sizeof(log_lines[0]), fmt, kept);
    pthread_mutex_unlock(&log_lock);
    va_end(kept);
    va_end(ap);
}

unsigned picocalc_sim_log_count(void)
{
    pthread_mutex_lock(&log_lock);
    unsigned n = log_total;
    pthread_mutex_unlock(&log_lock);
    return n;
}

bool picocalc_sim_log_line(unsigned i, char *buf, size_t cap)
{
    pthread_mutex_lock(&log_lock);
    bool kept = i < log_total && log_total - i <= LOG_KEEP;
    if (kept)
        snprintf(buf, cap, "%s", log_lines[i % LOG_KEEP]);
    pthread_mutex_unlock(&log_lock);
    return kept;
}

uint64_t picocalc_sim_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif

// Sleep() rounds up to the scheduler tick, 15.6 ms unless raised, which is far coarser than the
// throttle's sub-millisecond waits; Windows 10 1803 and later have timers that are not.
void picocalc_sim_sleep_ns(uint64_t ns)
{
    static _Thread_local HANDLE timer;
    static _Thread_local bool tried;
    if (ns == 0)
        return;
    if (!tried) {
        tried = true;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer)
            timeBeginPeriod(1);
    }
    LARGE_INTEGER due = {.QuadPart = -(LONGLONG)((ns + 99) / 100)};
    if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
        return;
    }
    Sleep((DWORD)((ns + 999999) / 1000000));
}

// The C runtime has no positioned I/O, but the file's own handle takes an offset with each call.
ssize_t pread(int fd, void *buf, size_t len, int64_t off)
{
    OVERLAPPED o = {.Offset = (DWORD)off, .OffsetHigh = (DWORD)(off >> 32)};
    DWORD got;
    if (ReadFile((HANDLE)_get_osfhandle(fd), buf, (DWORD)len, &got, &o))
        return (ssize_t)got;
    return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
}

ssize_t pwrite(int fd, const void *buf, size_t len, int64_t off)
{
    OVERLAPPED o = {.Offset = (DWORD)off, .OffsetHigh = (DWORD)(off >> 32)};
    DWORD put;
    return WriteFile((HANDLE)_get_osfhandle(fd), buf, (DWORD)len, &put, &o) ? (ssize_t)put : -1;
}
#else
void picocalc_sim_sleep_ns(uint64_t ns)
{
    if (ns == 0)
        return;
    struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}
#endif

// pcsim was this program's name before, and its data directory's; the first run under the new
// name moves that directory over, so the flash images, card and settings in it carry on.
static void adopt_old_data_dir(const char *old, const char *dir)
{
    static bool checked;
    struct stat st;
    if (checked)
        return;
    checked = true;
    if (stat(dir, &st) == 0 || stat(old, &st) != 0)
        return;
    if (rename(old, dir) == 0)
        picocalc_sim_log("moved the data directory %s to %s", old, dir);
    else
        picocalc_sim_log("cannot move the data directory %s to %s: %s", old, dir, strerror(errno));
}

void picocalc_sim_data_path(char *path, size_t cap, const char *file, bool create_dir)
{
    char base[3500], dir[3600], old[3600];
#ifdef _WIN32
    const char *app = getenv("LOCALAPPDATA");
    snprintf(base, sizeof(base), "%s", app && *app ? app : ".");
    snprintf(dir, sizeof(dir), "%s\\picocalc-sim", base);
    snprintf(old, sizeof(old), "%s\\pcsim", base);
    adopt_old_data_dir(old, dir);
    if (create_dir)
        picocalc_sim_mkdir(dir);
    snprintf(path, cap, "%s\\%s", dir, file);
#else
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(base, sizeof(base), "%s", xdg);
    else
        snprintf(base, sizeof(base), "%s/.local/share", home ? home : ".");
    snprintf(dir, sizeof(dir), "%s/picocalc-sim", base);
    snprintf(old, sizeof(old), "%s/pcsim", base);
    adopt_old_data_dir(old, dir);
    if (create_dir) {
        picocalc_sim_mkdir(base);
        picocalc_sim_mkdir(dir);
    }
    snprintf(path, cap, "%s/%s", dir, file);
#endif
}

void picocalc_sim_request_quit(int code)
{
    atomic_store(&quit_code, code);
    atomic_store(&quit_flag, true);
}

bool picocalc_sim_quit_requested(int *code)
{
    if (!atomic_load(&quit_flag))
        return false;
    if (code)
        *code = atomic_load(&quit_code);
    return true;
}

// UART output is what a USB-serial adapter on the PicoCalc's UART header would show.
void picocalc_sim_console_out(int uart, uint8_t byte)
{
    if (!picocalc_sim_opts.console || uart != 0)
        return;
    if (byte == '\r')
        return;
    fputc(byte, stdout);
    if (byte == '\n')
        fflush(stdout);
}
