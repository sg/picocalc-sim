#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#endif

#include "pcsim/port.h"
#include "pcsim/sim.h"

pcsim_options_t pcsim_opts = {
    .screenshot_dir = ".",
    .scale = 2,
    .battery = 85,
};

static atomic_bool quit_flag;
static atomic_int quit_code;

void pcsim_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("[pcsim] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

uint64_t pcsim_mono_ns(void)
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
void pcsim_sleep_ns(uint64_t ns)
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
void pcsim_sleep_ns(uint64_t ns)
{
    if (ns == 0)
        return;
    struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}
#endif

void pcsim_data_path(char *path, size_t cap, const char *file, bool create_dir)
{
    char dir[3600];
#ifdef _WIN32
    const char *base = getenv("LOCALAPPDATA");
    snprintf(dir, sizeof(dir), "%s\\pcsim", base && *base ? base : ".");
    if (create_dir)
        pcsim_mkdir(dir);
    snprintf(path, cap, "%s\\%s", dir, file);
#else
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(dir, sizeof(dir), "%s/pcsim", xdg);
    else
        snprintf(dir, sizeof(dir), "%s/.local/share/pcsim", home ? home : ".");
    if (create_dir) {
        char parent[sizeof(dir)];
        snprintf(parent, sizeof(parent), "%s", dir);
        char *slash = strrchr(parent, '/');
        if (slash) {
            *slash = 0;
            pcsim_mkdir(parent);
        }
        pcsim_mkdir(dir);
    }
    snprintf(path, cap, "%s/%s", dir, file);
#endif
}

void pcsim_request_quit(int code)
{
    atomic_store(&quit_code, code);
    atomic_store(&quit_flag, true);
}

bool pcsim_quit_requested(int *code)
{
    if (!atomic_load(&quit_flag))
        return false;
    if (code)
        *code = atomic_load(&quit_code);
    return true;
}

// UART output is what a USB-serial adapter on the PicoCalc's UART header would show.
void pcsim_console_out(int uart, uint8_t byte)
{
    if (!pcsim_opts.console || uart != 0)
        return;
    if (byte == '\r')
        return;
    fputc(byte, stdout);
    if (byte == '\n')
        fflush(stdout);
}
