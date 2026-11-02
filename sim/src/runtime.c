#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

void pcsim_sleep_ns(uint64_t ns)
{
    if (ns == 0)
        return;
    struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
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
