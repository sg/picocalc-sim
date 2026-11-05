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

#ifdef __cplusplus
extern "C" {
#endif

typedef struct picocalc_sim_options {
    const char *board;
    // The board for when neither board nor the UF2 names one; the window's last.
    const char *default_board;
    const char *flash_path;
    const char *rom_path;
    const char *card_path;
    const char *uf2_path;
    const char *script_path;
    const char *screenshot_dir;
    bool fast;
    bool unthrottled;
    bool console;
    bool headless;
    bool trace;
    bool erase_flash;
    int scale;
    double timeout_s;
    int battery;
    bool charging;
    bool no_wifi;
    const char *wifi_aps[16];
    int wifi_ap_count;
    const char *forwards[16];
    int forward_count;
    const char *wav_path;
    bool mute;
    int volume;
} picocalc_sim_options_t;

extern picocalc_sim_options_t picocalc_sim_opts;

void picocalc_sim_log(const char *fmt, ...) __attribute__((format(PICOCALC_SIM_PRINTF_FORMAT, 1, 2)));
// The most recent picocalc_sim_log lines, for a frontend to show: line i (counting from the first
// ever logged) is copied out if it is still kept.
unsigned picocalc_sim_log_count(void);
bool picocalc_sim_log_line(unsigned i, char *buf, size_t cap);

uint64_t picocalc_sim_mono_ns(void);
// Time since power-on on the emulated chip, which device models use for their own timing.
uint64_t picocalc_sim_uptime_us(void);
void picocalc_sim_sleep_ns(uint64_t ns);
// Builds the path of FILE in picocalc-sim's data directory: $XDG_DATA_HOME/picocalc-sim
// (~/.local/share/picocalc-sim) or %LOCALAPPDATA%\picocalc-sim on Windows.
void picocalc_sim_data_path(char *path, size_t cap, const char *file, bool create_dir);

typedef enum {
    PICOCALC_SIM_RUNNING,
    PICOCALC_SIM_BOOTSEL,
    PICOCALC_SIM_POWERED_OFF,
    PICOCALC_SIM_HALTED,
} picocalc_sim_state_t;

picocalc_sim_state_t picocalc_sim_state(void);
const char *picocalc_sim_state_reason(void);

// Requests the device models and the frontend make of the machine; all are thread-safe.
void picocalc_sim_reboot(const char *reason);
void picocalc_sim_stop(picocalc_sim_state_t state, const char *reason);
void picocalc_sim_power_button(void);
void picocalc_sim_bootsel_button(bool held);
// Writes a UF2 file into flash as BOOTSEL's mass-storage drive would, then reboots.
bool picocalc_sim_flash_uf2(const char *path, char *err, size_t errlen);

void picocalc_sim_request_quit(int code);
bool picocalc_sim_quit_requested(int *code);

// Held by the emulation thread while it runs; other threads take it to touch chip state.
void picocalc_sim_machine_lock(void);
void picocalc_sim_machine_unlock(void);

bool picocalc_sim_machine_start(void);
bool picocalc_sim_machine_running(void);
// Takes down everything picocalc_sim_machine_start set up, saving flash and finishing any
// recording.
void picocalc_sim_machine_stop(void);
// Stops the machine and starts it as a fresh launch with the current picocalc_sim_opts would,
// except that a flash of "none" keeps what it held. Call it from the frontend's thread while
// nothing else touches picocalc_sim_picocalc; false if the machine could not start, which leaves it
// stopped.
bool picocalc_sim_machine_restart(void);
// Asks the frontend to restart the machine when it next can; safe from any thread.
void picocalc_sim_request_restart(void);
bool picocalc_sim_restart_requested(void);
// How many times the machine has started, so a thread that asked for a restart can wait for it.
unsigned picocalc_sim_machine_starts(void);
const char *picocalc_sim_board_name(void);

typedef struct {
    const char *name;
    bool rp2040, wifi;
    unsigned flash_mb, psram_mb;
} picocalc_sim_board_desc_t;
// The boards --board accepts; false once i is past the last.
bool picocalc_sim_board_desc(int i, picocalc_sim_board_desc_t *d);
double picocalc_sim_speed(void);

void picocalc_sim_console_out(int uart, uint8_t byte);
void picocalc_sim_console_in(uint8_t byte);

#ifdef __cplusplus
}
#endif
