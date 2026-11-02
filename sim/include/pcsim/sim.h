#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pcsim_options {
    const char *board;
    const char *flash_path;
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
} pcsim_options_t;

extern pcsim_options_t pcsim_opts;

void pcsim_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

uint64_t pcsim_mono_ns(void);
// Time since power-on on the emulated chip, which device models use for their own timing.
uint64_t pcsim_uptime_us(void);
void pcsim_sleep_ns(uint64_t ns);

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
