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

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pcsim_options {
    const char *board;
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
} pcsim_options_t;

extern pcsim_options_t pcsim_opts;

void pcsim_log(const char *fmt, ...) __attribute__((format(PCSIM_PRINTF_FORMAT, 1, 2)));

uint64_t pcsim_mono_ns(void);
// Time since power-on on the emulated chip, which device models use for their own timing.
uint64_t pcsim_uptime_us(void);
void pcsim_sleep_ns(uint64_t ns);
// Builds the path of FILE in pcsim's data directory: $XDG_DATA_HOME/pcsim (~/.local/share/pcsim)
// or %LOCALAPPDATA%\pcsim on Windows.
void pcsim_data_path(char *path, size_t cap, const char *file, bool create_dir);

typedef enum {
    PCSIM_RUNNING,
    PCSIM_BOOTSEL,
    PCSIM_POWERED_OFF,
    PCSIM_HALTED,
} pcsim_state_t;

pcsim_state_t pcsim_state(void);
const char *pcsim_state_reason(void);

// Requests the device models and the frontend make of the machine; all are thread-safe.
void pcsim_reboot(const char *reason);
void pcsim_stop(pcsim_state_t state, const char *reason);
void pcsim_power_button(void);
void pcsim_bootsel_button(bool held);
// Writes a UF2 file into flash as BOOTSEL's mass-storage drive would, then reboots.
bool pcsim_flash_uf2(const char *path, char *err, size_t errlen);

void pcsim_request_quit(int code);
bool pcsim_quit_requested(int *code);

// Held by the emulation thread while it runs; other threads take it to touch chip state.
void pcsim_machine_lock(void);
void pcsim_machine_unlock(void);

bool pcsim_machine_start(void);
void pcsim_machine_stop(void);
const char *pcsim_board_name(void);
double pcsim_speed(void);

void pcsim_console_out(int uart, uint8_t byte);
void pcsim_console_in(uint8_t byte);

void pcsim_flush_devices(void);

#ifdef __cplusplus
}
#endif
