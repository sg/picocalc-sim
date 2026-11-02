#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uf2.h"

#define UF2_MAGIC0 0x0a324655u
#define UF2_MAGIC1 0x9e5d5157u
#define UF2_MAGIC_END 0x0ab16f30u
#define UF2_FLAG_NOT_MAIN_FLASH 0x00000001u
#define UF2_FLAG_FAMILY_ID 0x00002000u

bool uf2_load(const char *path, uf2_file_t *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "cannot open %s", path);
        return false;
    }
    uint8_t b[512];
    size_t cap = 0;
    while (fread(b, 1, 512, f) == 512) {
        if (rd32le(b) != UF2_MAGIC0 || rd32le(b + 4) != UF2_MAGIC1 || rd32le(b + 508) != UF2_MAGIC_END)
            continue;
        uint32_t flags = rd32le(b + 8), addr = rd32le(b + 12), len = rd32le(b + 16);
        uint32_t family = (flags & UF2_FLAG_FAMILY_ID) ? rd32le(b + 28) : 0;
        if ((flags & UF2_FLAG_NOT_MAIN_FLASH) || len > 256)
            continue;
        if (out->count == cap) {
            cap = cap ? cap * 2 : 1024;
            out->blocks = realloc(out->blocks, cap * sizeof(uf2_block_t));
        }
        uf2_block_t *blk = &out->blocks[out->count++];
        blk->addr = addr;
        blk->family = family;
        blk->len = len;
        memcpy(blk->data, b + 32, len);
        // The absolute-family block at the top of a 16 MB window is the RP2350-E10 workaround,
        // not part of the image.
        if (family && !(family == UF2_FAMILY_ABSOLUTE && addr >= 0x10ffff00u))
            if (!out->family || out->family == UF2_FAMILY_ABSOLUTE)
                out->family = family;
        if ((addr >> 28) == 2)
            out->ram = true;
    }
    fclose(f);
    if (!out->count) {
        snprintf(err, errlen, "%s is not a UF2 file", path);
        return false;
    }
    return true;
}

void uf2_free(uf2_file_t *f)
{
    free(f->blocks);
    memset(f, 0, sizeof(*f));
}

static const uf2_block_t *find_block(const uf2_file_t *f, uint32_t addr)
{
    for (size_t i = 0; i < f->count; i++)
        if (addr >= f->blocks[i].addr && addr < f->blocks[i].addr + f->blocks[i].len)
            return &f->blocks[i];
    return NULL;
}

static bool read32(const uf2_file_t *f, uint32_t addr, uint32_t *v)
{
    uint8_t bytes[4];
    for (int i = 0; i < 4; i++) {
        const uf2_block_t *b = find_block(f, addr + (uint32_t)i);
        if (!b)
            return false;
        bytes[i] = b->data[addr + (uint32_t)i - b->addr];
    }
    *v = rd32le(bytes);
    return true;
}

static bool read_str(const uf2_file_t *f, uint32_t addr, char *out, size_t cap)
{
    for (size_t i = 0; i + 1 < cap; i++) {
        const uf2_block_t *b = find_block(f, addr + (uint32_t)i);
        if (!b)
            return false;
        out[i] = (char)b->data[addr + (uint32_t)i - b->addr];
        if (!out[i])
            return true;
    }
    out[cap - 1] = 0;
    return true;
}

// Binary info: a header of markers and table pointers near the start of the image lists entries;
// the one with ID BINARY_INFO_ID_RP_PICO_BOARD names the board.
const char *uf2_board_hint(const uf2_file_t *f)
{
    static char name[64];
    if (!f->count)
        return NULL;
    uint32_t base = UINT32_MAX;
    for (size_t i = 0; i < f->count; i++)
        if (f->blocks[i].addr < base)
            base = f->blocks[i].addr;
    for (uint32_t a = base; a < base + 0x1000; a += 4) {
        uint32_t marker;
        if (!read32(f, a, &marker) || marker != 0x7188ebf2u)
            continue;
        uint32_t start, end, endmark;
        if (!read32(f, a + 4, &start) || !read32(f, a + 8, &end) || !read32(f, a + 16, &endmark) ||
            endmark != 0xe71aa390u)
            continue;
        for (uint32_t p = start; p < end && p < start + 0x400; p += 4) {
            uint32_t ent, hdr, id, str;
            if (!read32(f, p, &ent) || !read32(f, ent, &hdr) || !read32(f, ent + 4, &id))
                continue;
            if ((hdr & 0xffff) != 6 || id != 0xb63cffbbu)
                continue;
            if (read32(f, ent + 8, &str) && read_str(f, str, name, sizeof(name)))
                return name;
        }
    }
    return NULL;
}

// The partition a UF2 of this family lands in, by the partition table already in flash.
static bool target_partition(chip_t *m, uint32_t family, uint32_t *start, uint32_t *size)
{
    uint32_t bit;
    switch (family) {
    case UF2_FAMILY_RP2040: bit = 1u << 14; break;
    case UF2_FAMILY_ABSOLUTE: return false;
    case UF2_FAMILY_DATA: bit = 1u << 16; break;
    case UF2_FAMILY_RP2350_ARM_S: bit = 1u << 17; break;
    case UF2_FAMILY_RP2350_RISCV: bit = 1u << 18; break;
    case UF2_FAMILY_RP2350_ARM_NS: bit = 1u << 19; break;
    default: return false;
    }
    for (uint32_t off = 0; off < 0x1000; off += 4) {
        if (rd32le(m->flash + off) != 0xffffded3u)
            continue;
        uint32_t q = off + 4;
        for (int n = 0; n < 64 && q + 8 < 0x1400; n++) {
            uint32_t hdr = rd32le(m->flash + q);
            unsigned type = hdr & 0xff;
            uint32_t sz = (type & 0x80) ? (hdr >> 8) & 0xffff : (hdr >> 8) & 0xff;
            if ((type & 0x7f) == 0x7f || !sz)
                break;
            if (type == 0x0a) {
                int count = (int)((hdr >> 24) & 0x0f);
                uint32_t p = q + 8;
                for (int i = 0; i < count; i++) {
                    uint32_t loc = rd32le(m->flash + p), flags = rd32le(m->flash + p + 4);
                    p += 8;
                    if (flags & 1)
                        p += 8;
                    int nfam = (int)((flags >> 7) & 3);
                    bool accepts = (flags & bit) != 0;
                    for (int k = 0; k < nfam; k++) {
                        if (rd32le(m->flash + p) == family)
                            accepts = true;
                        p += 4;
                    }
                    if (flags & (1u << 12)) {
                        uint32_t len = rd32le(m->flash + p) & 0x7f;
                        p += (len + 1 + 3) & ~3u;
                    }
                    if (accepts) {
                        *start = (loc & 0x1fff) * 0x1000;
                        *size = (((loc >> 13) & 0x1fff) + 1) * 0x1000 - *start;
                        return true;
                    }
                }
                return false;
            }
            q += sz * 4;
        }
    }
    return false;
}

bool uf2_program(chip_t *m, const uf2_file_t *f, char *err, size_t errlen)
{
    if (f->family == UF2_FAMILY_RP2040) {
        snprintf(err, errlen, "this UF2 is for the RP2040 (Pico/Pico W), not the RP2350 on a %s", m->board->name);
        return false;
    }
    if (f->family == UF2_FAMILY_RP2350_RISCV) {
        snprintf(err, errlen, "RISC-V images are not supported yet");
        return false;
    }
    if (f->ram) {
        uint32_t lo = UINT32_MAX, hi = 0;
        for (size_t i = 0; i < f->count; i++) {
            const uf2_block_t *b = &f->blocks[i];
            if ((b->addr >> 28) != 2)
                continue;
            for (uint32_t k = 0; k < b->len; k++)
                sys_write(m, b->addr + k, b->data[k], 1, NULL);
            if (b->addr < lo)
                lo = b->addr;
            if (b->addr + b->len > hi)
                hi = b->addr + b->len;
        }
        m->boot_type = 3;
        m->boot_param[0] = lo;
        m->boot_param[1] = hi - lo;
        return true;
    }
    uint32_t pstart = 0, psize = m->flash_size;
    bool in_partition = target_partition(m, f->family, &pstart, &psize);
    static uint8_t erased[4096];
    memset(erased, 0, sizeof(erased));
    for (size_t i = 0; i < f->count; i++) {
        const uf2_block_t *b = &f->blocks[i];
        if ((b->addr >> 24) != 0x10)
            continue;
        uint32_t off = b->addr - XIP_BASE;
        if (b->family == UF2_FAMILY_ABSOLUTE && b->addr >= 0x10ffff00u)
            continue;
        if (in_partition && b->family != UF2_FAMILY_ABSOLUTE) {
            if (off + b->len > psize) {
                snprintf(err, errlen, "image is larger than its %u KB partition", psize / 1024);
                return false;
            }
            off += pstart;
        }
        if (off + b->len > m->flash_size) {
            snprintf(err, errlen, "image does not fit in %u MB of flash", m->flash_size >> 20);
            return false;
        }
        for (uint32_t s = off >> 12; s <= (off + b->len - 1) >> 12; s++) {
            if (!(erased[s >> 3] & (1u << (s & 7)))) {
                erased[s >> 3] |= (uint8_t)(1u << (s & 7));
                flash_erase(m, s << 12, 0x1000);
            }
        }
        flash_program(m, off, b->data, b->len);
    }
    m->boot_type = 0;
    xip_flush(m);
    return true;
}
