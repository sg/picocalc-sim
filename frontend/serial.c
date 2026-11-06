#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#include "picocalc-sim/sim.h"

// The terminal picocalc-sim runs in as the far end of the firmware's USB serial port, as a serial
// terminal program on the computer would be: what the firmware sends is printed, and what is typed
// goes to the firmware key by key and unechoed, since the firmware echoes it. Ctrl+] quits, as in
// telnet, so that Ctrl+C reaches the firmware.

#define QUIT_KEY 0x1d

static bool out_tty, in_tty, raw;

#ifdef _WIN32
static HANDLE in_handle, out_handle;
static DWORD saved_in_mode, saved_out_mode;
#else
static struct termios saved;
#endif

static void restore(void)
{
    if (!raw)
        return;
    raw = false;
#ifdef _WIN32
    SetConsoleMode(in_handle, saved_in_mode);
    SetConsoleMode(out_handle, saved_out_mode);
#else
    tcsetattr(0, TCSAFLUSH, &saved);
#endif
}

#ifndef _WIN32
static void on_signal(int sig)
{
    restore();
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

static void make_raw(void)
{
#ifdef _WIN32
    in_handle = GetStdHandle(STD_INPUT_HANDLE);
    out_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(in_handle, &saved_in_mode))
        return;
    // Keys arrive as the escape sequences a serial terminal sends, Ctrl+C among them.
    if (!SetConsoleMode(in_handle, ENABLE_VIRTUAL_TERMINAL_INPUT))
        SetConsoleMode(in_handle, 0);
    if (GetConsoleMode(out_handle, &saved_out_mode))
        SetConsoleMode(out_handle, saved_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    raw = true;
#else
    if (tcgetattr(0, &saved) != 0)
        return;
    struct termios t = saved;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
    t.c_iflag &= ~(tcflag_t)(ICRNL | INLCR | IGNCR | IXON | ISTRIP);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSAFLUSH, &t) != 0)
        return;
    raw = true;
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGQUIT, on_signal);
#endif
    atexit(restore);
}

void picocalc_sim_usb_serial_out(const uint8_t *data, size_t len)
{
    if (!picocalc_sim_opts.usb_console)
        return;
    // A log or a pipe gets lines without the carriage returns a terminal needs.
    if (out_tty)
        fwrite(data, 1, len, stdout);
    else
        for (size_t i = 0; i < len; i++)
            if (data[i] != '\r')
                fputc(data[i], stdout);
    fflush(stdout);
}

static void *reader(void *arg)
{
    (void)arg;
    uint8_t buf[256];
    for (;;) {
        int n = (int)read(0, buf, sizeof(buf));
        if (n <= 0)
            return NULL;
        size_t len = (size_t)n;
        const uint8_t *quit = raw ? memchr(buf, QUIT_KEY, len) : NULL;
        if (quit)
            len = (size_t)(quit - buf);
        // Typing reaches the port only while the host has it open, as with a terminal program; piped
        // input waits for it to open.
        for (size_t off = 0; off < len && !picocalc_sim_quit_requested(NULL);) {
            size_t k = picocalc_sim_usb_serial_send(buf + off, len - off);
            off += k;
            if (!k && in_tty && !picocalc_sim_usb_serial_is_open())
                break;
            if (!k)
                picocalc_sim_sleep_ns(10 * 1000000ull);
        }
        if (quit) {
            picocalc_sim_request_quit(0);
            return NULL;
        }
    }
}

void picocalc_sim_usb_console_start(void)
{
    out_tty = isatty(1);
    in_tty = isatty(0);
#ifdef _WIN32
    if (!in_tty)
        _setmode(0, _O_BINARY);
#else
    // A terminal read from the background would stop picocalc-sim, so it then only prints.
    if (in_tty && tcgetpgrp(0) != getpgrp())
        return;
#endif
    if (in_tty)
        make_raw();
    pthread_t t;
    if (pthread_create(&t, NULL, reader, NULL) == 0)
        pthread_detach(t);
    if (raw)
        picocalc_sim_log("usb: this terminal is the USB serial port; Ctrl+] quits");
}
