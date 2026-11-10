// A check of programming flash through the boot ROM's routines, which the SDK, UF2 Loader and
// PicoShell's loader all use, for picocalc-sim's CI: what they write must read back, and a second
// program of a page can only clear bits, as on the chip.
#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#define OFFSET (1u << 20)
#define SIZE (64u * 1024)

static uint8_t buf[SIZE], again[FLASH_PAGE_SIZE];
static int failed;

#define CHECK(ok, ...)                     \
    do {                                   \
        if (!(ok)) {                       \
            printf("FLASH FAIL: ");        \
            printf(__VA_ARGS__);           \
            printf("\n");                  \
            failed = 1;                    \
        }                                  \
    } while (0)

static int first_difference(const uint8_t *flash, const uint8_t *want, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (flash[i] != want[i])
            return (int)i;
    return -1;
}

int main(void)
{
    stdio_init_all();
    uint32_t x = 0x2350f1a5u;
    for (size_t i = 0; i < SIZE; i++) {
        x = x * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(x >> 24);
    }
    const uint8_t *flash = (const uint8_t *)(XIP_BASE + OFFSET);

    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(OFFSET, SIZE);
    restore_interrupts(irq);
    bool erased = true;
    for (size_t i = 0; i < SIZE && erased; i++)
        erased = flash[i] == 0xff;
    CHECK(erased, "erased flash does not read as ff");

    irq = save_and_disable_interrupts();
    flash_range_program(OFFSET, buf, SIZE);
    restore_interrupts(irq);
    int at = first_difference(flash, buf, SIZE);
    CHECK(at < 0, "programmed flash differs at %d: %02x, expected %02x", at, at < 0 ? 0 : flash[at],
          at < 0 ? 0 : buf[at]);

    for (size_t i = 0; i < FLASH_PAGE_SIZE; i++)
        again[i] = (uint8_t)~(i * 7);
    irq = save_and_disable_interrupts();
    flash_range_program(OFFSET, again, FLASH_PAGE_SIZE);
    restore_interrupts(irq);
    for (size_t i = 0; i < FLASH_PAGE_SIZE; i++)
        again[i] &= buf[i];
    at = first_difference(flash, again, FLASH_PAGE_SIZE);
    CHECK(at < 0, "a page programmed twice differs at %d: %02x, expected %02x", at, at < 0 ? 0 : flash[at],
          at < 0 ? 0 : again[at]);

    if (!failed)
        printf("FLASH PASS: erased, programmed and reprogrammed %u KB through the boot ROM\n", SIZE / 1024);
    for (;;)
        tight_loop_contents();
}
