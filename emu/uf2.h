#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "chip.h"

#define UF2_FAMILY_RP2040 0xe48bff56u
#define UF2_FAMILY_ABSOLUTE 0xe48bff57u
#define UF2_FAMILY_DATA 0xe48bff58u
#define UF2_FAMILY_RP2350_ARM_S 0xe48bff59u
#define UF2_FAMILY_RP2350_RISCV 0xe48bff5au
#define UF2_FAMILY_RP2350_ARM_NS 0xe48bff5bu

typedef struct {
    uint32_t addr;
    uint32_t family;
    uint8_t data[256];
    uint32_t len;
} uf2_block_t;

typedef struct {
    uf2_block_t *blocks;
    size_t count;
    uint32_t family;
    bool ram;
} uf2_file_t;

bool uf2_load(const char *path, uf2_file_t *out, char *err, size_t errlen);
void uf2_free(uf2_file_t *f);
// The board the image was built for, from its binary info ("pico2_w" and so on), or NULL.
const char *uf2_board_hint(const uf2_file_t *f);
// Writes the image as the boot ROM's mass-storage download would; returns false with a reason.
bool uf2_program(chip_t *m, const uf2_file_t *f, char *err, size_t errlen);
