#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "chip.h"

// A replacement for the RP2350 boot ROM. The ROM image holds the well-known header, a vector
// table and one stub per API function; each stub is an undefined instruction whose number the
// CPU hands to rom_hle(), which does the work in C and returns to the caller. The boot path
// follows the real ROM: watchdog and reboot parameters, then the block loop at the start of
// flash, an embedded or standalone partition table, address translation, load maps, and BOOTSEL
// when nothing bootable is found.

#define BLOCK_START 0xffffded3u
#define BLOCK_END 0xab123579u

#define ROM_STUBS 0x0100u
#define ROM_DATA 0x0800u
#define DEVINFO_ADDR 0x400e03f0u
#define RESIDENT_PT_ADDR 0x400e0300u

enum {
    HLE_CORE_START,
    HLE_TABLE_LOOKUP,
    HLE_DEAD,
    HLE_FLASH_ENTER_CMD_XIP,
    HLE_FLASH_EXIT_XIP,
    HLE_FLASH_FLUSH_CACHE,
    HLE_CONNECT_INTERNAL_FLASH,
    HLE_FLASH_RANGE_ERASE,
    HLE_FLASH_RANGE_PROGRAM,
    HLE_FLASH_SELECT_XIP_READ_MODE,
    HLE_FLASH_OP,
    HLE_FLASH_RUNTIME_TO_STORAGE,
    HLE_GET_SYS_INFO,
    HLE_GET_PARTITION_TABLE_INFO,
    HLE_LOAD_PARTITION_TABLE,
    HLE_PICK_AB_PARTITION,
    HLE_GET_B_PARTITION,
    HLE_GET_UF2_TARGET_PARTITION,
    HLE_CHAIN_IMAGE,
    HLE_EXPLICIT_BUY,
    HLE_FLASH_RESET_ADDRESS_TRANS,
    HLE_REBOOT,
    HLE_SET_ROM_CALLBACK,
    HLE_SECURE_CALL,
    HLE_SET_NS_API_PERMISSION,
    HLE_BOOTROM_STATE_RESET,
    HLE_SET_BOOTROM_STACK,
    HLE_VALIDATE_NS_BUFFER,
    HLE_OTP_ACCESS,
    HLE_COUNT,
};

#define CODE(a, b) ((uint32_t)(a) | ((uint32_t)(b) << 8))

static const struct {
    uint32_t code;
    int hle;
} func_table[] = {
    {CODE('C', 'X'), HLE_FLASH_ENTER_CMD_XIP},
    {CODE('E', 'X'), HLE_FLASH_EXIT_XIP},
    {CODE('F', 'C'), HLE_FLASH_FLUSH_CACHE},
    {CODE('I', 'F'), HLE_CONNECT_INTERNAL_FLASH},
    {CODE('R', 'E'), HLE_FLASH_RANGE_ERASE},
    {CODE('R', 'P'), HLE_FLASH_RANGE_PROGRAM},
    {CODE('X', 'M'), HLE_FLASH_SELECT_XIP_READ_MODE},
    {CODE('F', 'O'), HLE_FLASH_OP},
    {CODE('F', 'A'), HLE_FLASH_RUNTIME_TO_STORAGE},
    {CODE('G', 'S'), HLE_GET_SYS_INFO},
    {CODE('G', 'P'), HLE_GET_PARTITION_TABLE_INFO},
    {CODE('L', 'P'), HLE_LOAD_PARTITION_TABLE},
    {CODE('A', 'B'), HLE_PICK_AB_PARTITION},
    {CODE('G', 'B'), HLE_GET_B_PARTITION},
    {CODE('G', 'U'), HLE_GET_UF2_TARGET_PARTITION},
    {CODE('C', 'I'), HLE_CHAIN_IMAGE},
    {CODE('E', 'B'), HLE_EXPLICIT_BUY},
    {CODE('R', 'A'), HLE_FLASH_RESET_ADDRESS_TRANS},
    {CODE('R', 'B'), HLE_REBOOT},
    {CODE('R', 'C'), HLE_SET_ROM_CALLBACK},
    {CODE('S', 'C'), HLE_SECURE_CALL},
    {CODE('S', 'P'), HLE_SET_NS_API_PERMISSION},
    {CODE('S', 'R'), HLE_BOOTROM_STATE_RESET},
    {CODE('S', 'S'), HLE_SET_BOOTROM_STACK},
    {CODE('V', 'B'), HLE_VALIDATE_NS_BUFFER},
    {CODE('O', 'A'), HLE_OTP_ACCESS},
};

static uint32_t stub_addr(int hle)
{
    return ROM_STUBS + 4u * (uint32_t)hle;
}

static void forget_state(void);

void rom_build(chip_t *m)
{
    forget_state();
    uint8_t *r = m->rom;
    memset(r, 0, ROM_SIZE);
    wr32le(r + 0x00, 0x20082000u);
    wr32le(r + 0x04, stub_addr(HLE_CORE_START) | 1);
    wr32le(r + 0x08, stub_addr(HLE_DEAD) | 1);
    wr32le(r + 0x0c, stub_addr(HLE_DEAD) | 1);
    r[0x10] = 'M';
    r[0x11] = 'u';
    r[0x12] = 2;
    r[0x13] = 2;
    wr16le(r + 0x14, (uint16_t)ROM_DATA);
    wr16le(r + 0x16, (uint16_t)(stub_addr(HLE_TABLE_LOOKUP) | 1));
    for (int i = 0; i < HLE_COUNT; i++) {
        wr16le(r + stub_addr(i), (uint16_t)(0xde00 | i));
        wr16le(r + stub_addr(i) + 2, 0x4770);
    }
    // Data the ROM exposes: its git revision and the RAM-resident words that describe the
    // flash devices and partition table.
    wr32le(r + ROM_DATA + 0x00, 0x312e0eb5u);
    wr32le(r + ROM_DATA + 0x04, 0x400e0001u);
    wr32le(r + ROM_DATA + 0x08, DEVINFO_ADDR);
}

bool rp2350_load_rom(chip_t *m, const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno));
        return false;
    }
    uint8_t image[ROM_SIZE + 1];
    size_t n = fread(image, 1, sizeof(image), f);
    fclose(f);
    if (n != ROM_SIZE || image[0x10] != 'M' || image[0x11] != 'u' || image[0x12] != 2) {
        snprintf(err, errlen, "%s is not a 32 KB RP2350 boot ROM image such as bootrom-combined.bin", path);
        return false;
    }
    // Its minor version is the stepping it was built for, which the chip becomes; A4 changed only
    // the ROM.
    if (image[0x13] < 2 || image[0x13] > 4) {
        snprintf(err, errlen, "%s is a boot ROM for RP2350-A%u, which is not emulated; A2, A3 and A4 are", path,
                 image[0x13]);
        return false;
    }
    memcpy(m->rom, image, ROM_SIZE);
    memcpy(m->rom + ROM_SIZE, image, ROM_SIZE);
    m->real_rom = true;
    m->a3 = image[0x13] >= 3;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Memory access for the ROM, which bypasses the CPU's bus timing.

static uint32_t mr32(chip_t *m, uint32_t a)
{
    return sys_read(m, a, 4, NULL);
}

static uint8_t mr8(chip_t *m, uint32_t a)
{
    return (uint8_t)sys_read(m, a, 1, NULL);
}

static void mw32(chip_t *m, uint32_t a, uint32_t v)
{
    sys_write(m, a, v, 4, NULL);
}

static void mw8(chip_t *m, uint32_t a, uint8_t v)
{
    sys_write(m, a, v, 1, NULL);
}

static void charge(cpu_t *c, uint64_t ps)
{
    c->t += ps;
    // A predecoded block keeps the time in a register until told to look again.
    c->check_exc = true;
}

// ---------------------------------------------------------------------------------------------
// Picobin blocks

typedef struct {
    uint32_t addr;
    bool image_def, partition_table;
    uint32_t image_type;
    bool has_vt, has_ep;
    uint32_t vt, ep_pc, ep_sp;
    uint32_t load_map, pt_item;
    int32_t link;
} block_t;

// Region readers work on storage offsets for flash and on bus addresses for RAM.
typedef struct {
    chip_t *m;
    bool flash;
    uint32_t base;
    uint32_t size;
} region_t;

static uint32_t rgn32(region_t *rg, uint32_t off)
{
    if (off + 4 > rg->size)
        return 0;
    if (rg->flash) {
        uint32_t o = rg->base + off;
        if (o + 4 > rg->m->flash_size)
            return 0;
        return rd32le(rg->m->flash + o);
    }
    return mr32(rg->m, rg->base + off);
}

static bool parse_block(region_t *rg, uint32_t off, block_t *b)
{
    memset(b, 0, sizeof(*b));
    b->addr = off;
    if (rgn32(rg, off) != BLOCK_START)
        return false;
    uint32_t q = off + 4;
    for (int n = 0; n < 64; n++) {
        uint32_t hdr = rgn32(rg, q);
        unsigned type = hdr & 0xff;
        uint32_t size = (type & 0x80) ? (hdr >> 8) & 0xffff : (hdr >> 8) & 0xff;
        if ((type & 0x7f) == 0x7f) {
            b->link = (int32_t)rgn32(rg, q + 4);
            return rgn32(rg, q + 8) == BLOCK_END;
        }
        if (!size || q - off > 0x280)
            return false;
        switch (type & 0x7f) {
        case 0x42:
            b->image_type = hdr >> 16;
            if ((b->image_type & 0xf) == 1)
                b->image_def = true;
            break;
        case 0x03:
            b->has_vt = true;
            b->vt = rgn32(rg, q + 4);
            break;
        case 0x44:
            b->has_ep = true;
            b->ep_pc = rgn32(rg, q + 4);
            b->ep_sp = rgn32(rg, q + 8);
            break;
        case 0x06:
            b->load_map = q;
            break;
        case 0x0a:
            b->partition_table = true;
            b->pt_item = q;
            break;
        default:
            break;
        }
        q += size * 4;
    }
    return false;
}

// Finds the block loop in the first 4 KB of a region and collects the IMAGE_DEF and
// PARTITION_TABLE blocks in it.
static bool find_blocks(region_t *rg, block_t *image, block_t *pt)
{
    bool have_image = false, have_pt = false;
    for (uint32_t off = 0; off < 0x1000 && off < rg->size; off += 4) {
        if (rgn32(rg, off) != BLOCK_START)
            continue;
        block_t b;
        if (!parse_block(rg, off, &b))
            continue;
        uint32_t first = off, cur = off;
        for (int hops = 0; hops < 16; hops++) {
            if (b.image_def && (((b.image_type >> 8) & 7) == 0) && !have_image) {
                *image = b;
                have_image = true;
            }
            if (b.partition_table && !have_pt) {
                *pt = b;
                have_pt = true;
            }
            uint32_t next = (uint32_t)((int32_t)cur + b.link);
            if (next == first || b.link == 0)
                break;
            if (!parse_block(rg, next, &b))
                break;
            cur = next;
        }
        return have_image || have_pt;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Partition table

typedef struct {
    uint32_t loc, flags;
    uint32_t id[2];
    uint32_t families[3];
    int nfamilies;
    char name[64];
} partition_t;

typedef struct {
    bool loaded, present;
    uint32_t unpartitioned_flags;
    int count;
    partition_t p[16];
} ptable_t;

static ptable_t pt;

static void parse_partition_table(region_t *rg, block_t *b)
{
    memset(&pt, 0, sizeof(pt));
    pt.loaded = true;
    if (!b || !b->partition_table)
        return;
    uint32_t q = b->pt_item;
    uint32_t hdr = rgn32(rg, q);
    int count = (int)((hdr >> 24) & 0x0f);
    pt.present = true;
    pt.unpartitioned_flags = rgn32(rg, q + 4);
    q += 8;
    for (int i = 0; i < count && i < 16; i++) {
        partition_t *p = &pt.p[i];
        p->loc = rgn32(rg, q);
        p->flags = rgn32(rg, q + 4);
        q += 8;
        if (p->flags & 1) {
            p->id[0] = rgn32(rg, q);
            p->id[1] = rgn32(rg, q + 4);
            q += 8;
        }
        p->nfamilies = (int)((p->flags >> 7) & 3);
        for (int k = 0; k < p->nfamilies; k++) {
            p->families[k] = rgn32(rg, q);
            q += 4;
        }
        if (p->flags & (1u << 12)) {
            uint32_t w = rgn32(rg, q);
            unsigned len = w & 0x7f;
            for (unsigned k = 0; k < len && k < sizeof(p->name) - 1; k++) {
                uint32_t word = rgn32(rg, q + ((k + 1) & ~3u));
                p->name[k] = (char)(word >> (8 * ((k + 1) & 3)));
            }
            q += (len + 1 + 3) & ~3u;
        }
    }
    pt.count = count;
}

static uint32_t part_start(const partition_t *p)
{
    return (p->loc & 0x1fff) * 0x1000;
}

static uint32_t part_end(const partition_t *p)
{
    return (((p->loc >> 13) & 0x1fff) + 1) * 0x1000;
}

static void load_pt_from_flash(chip_t *m)
{
    // The loop's first block lies in the first 4 KB, but its links may lead anywhere in flash.
    region_t rg = {m, true, 0, m->flash_size};
    block_t image, ptb;
    memset(&ptb, 0, sizeof(ptb));
    if (find_blocks(&rg, &image, &ptb) && ptb.partition_table)
        parse_partition_table(&rg, &ptb);
    else
        parse_partition_table(&rg, NULL);
}

// ---------------------------------------------------------------------------------------------
// Address translation

static void atrans_identity(chip_t *m)
{
    for (int i = 0; i < 4; i++)
        m->qmi.atrans[i] = (0x400u << 16) | ((uint32_t)i * 0x400u);
    chip_rebuild_pages(m);
}

static void atrans_map(chip_t *m, uint32_t storage, uint32_t size)
{
    for (int i = 0; i < 4; i++) {
        uint32_t woff = (uint32_t)i * 0x400000u;
        uint32_t wsize = size > woff ? size - woff : 0;
        if (wsize > 0x400000u)
            wsize = 0x400000u;
        m->qmi.atrans[i] = ((wsize >> 12) << 16) | (((storage + woff) >> 12) & 0xfff);
    }
    chip_rebuild_pages(m);
}

static int runtime_to_storage(chip_t *m, uint32_t addr, uint32_t *out)
{
    if ((addr >> 24) != 0x10 && (addr >> 24) != 0x14) {
        if ((addr >> 24) == 0x1c) {
            *out = addr & 0x00ffffffu;
            return 0;
        }
        return -10;
    }
    uint32_t off = addr & 0x00ffffffu;
    uint32_t at = m->qmi.atrans[off >> 22];
    uint32_t size = ((at >> 16) & 0x7ff) << 12, base = (at & 0xfff) << 12;
    if ((off & 0x3fffff) >= size)
        return -10;
    *out = base + (off & 0x3fffff);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Launching images

static void launch(cpu_t *c, uint32_t vt, uint32_t sp, uint32_t pc)
{
    c->vtor = vt;
    c->msp = sp & ~3u;
    c->r[13] = c->msp;
    c->control = 0;
    c->primask = 0;
    c->r[14] = 0xffffffffu;
    c->r[15] = pc & ~1u;
    c->state = CPU_RUN;
    c->check_exc = true;
}

static void apply_load_map(chip_t *m, region_t *rg, block_t *b, uint32_t runtime_base)
{
    if (!b->load_map)
        return;
    uint32_t hdr = rgn32(rg, b->load_map);
    bool absolute = hdr & 0x80000000u;
    unsigned n = (hdr >> 24) & 0x7f;
    for (unsigned i = 0; i < n; i++) {
        uint32_t q = b->load_map + 4 + 12 * i;
        uint32_t s = rgn32(rg, q), dst = rgn32(rg, q + 4), third = rgn32(rg, q + 8);
        uint32_t size;
        uint32_t src_off;
        if (absolute) {
            size = third - dst;
            if (s == 0) {
                src_off = 0;
            } else if (rg->flash) {
                uint32_t st;
                if (runtime_to_storage(m, s, &st) != 0)
                    st = s & 0x00ffffffu;
                src_off = st;
            } else {
                src_off = s;
            }
        } else {
            size = third;
            src_off = s ? (rg->flash ? rg->base + b->addr + s : rg->base + b->addr + s) : 0;
        }
        if (dst == runtime_base || (dst >> 28) == 1)
            continue;
        for (uint32_t k = 0; k < size; k += 4) {
            uint32_t v = 0;
            if (s) {
                if (rg->flash)
                    v = src_off + k + 4 <= m->flash_size ? rd32le(m->flash + src_off + k) : 0;
                else
                    v = mr32(m, src_off + k);
            }
            mw32(m, dst + k, v);
        }
    }
}

static bool try_launch_region(cpu_t *c, region_t *rg, uint32_t runtime_base, bool map_flash)
{
    chip_t *m = c->m;
    block_t image, ptb;
    memset(&image, 0, sizeof(image));
    memset(&ptb, 0, sizeof(ptb));
    if (!find_blocks(rg, &image, &ptb) || !image.image_def)
        return false;
    if (map_flash)
        atrans_map(m, rg->base, rg->size);
    apply_load_map(m, rg, &image, runtime_base);
    uint32_t vt = image.has_vt ? image.vt : runtime_base;
    uint32_t sp, pc;
    if (image.has_ep) {
        pc = image.ep_pc;
        sp = image.ep_sp;
    } else {
        sp = mr32(m, vt);
        pc = mr32(m, vt + 4);
    }
    if (m->trace)
        emu_log("rom: launching image at %08x (vt %08x sp %08x pc %08x)", runtime_base, vt, sp, pc);
    launch(c, vt, sp, pc);
    return true;
}

static void enter_bootsel(cpu_t *c, const char *why)
{
    chip_t *m = c->m;
    m->boot_type = 2;
    chip_request(m, REQ_BOOTSEL, why);
    c->state = CPU_WFE;
    c->r[15] = stub_addr(HLE_DEAD);
}

static void boot_core0(cpu_t *c)
{
    chip_t *m = c->m;
    // The ROM leaves its XIP setup routine at the start of boot RAM, and the SDK copies and calls
    // it to restore XIP after programming flash. XIP never stops working here, so it just returns.
    wr16le(m->bootram, 0x4770);
    // FLASH_DEVINFO as unprogrammed OTP leaves it: a 16 MB device on CS0 and nothing on CS1.
    if (m->boot_count <= 1 || !rd16le(m->bootram + (DEVINFO_ADDR - 0x400e0000u)))
        wr16le(m->bootram + (DEVINFO_ADDR - 0x400e0000u), 12u << 8);
    if (m->wd_scratch[4] == 0xb007c0d3u && (m->wd_scratch[4] ^ (uint32_t)-(int32_t)m->wd_scratch[7]) == m->wd_scratch[5]) {
        uint32_t pc = m->wd_scratch[7], sp = m->wd_scratch[6];
        m->wd_scratch[4] = 0;
        m->boot_type = 0xd;
        launch(c, c->vtor, sp, pc);
        return;
    }
    uint32_t type = m->boot_type & 0xf;
    m->rom_partition = -1;
    pt.loaded = false;
    if (type == 2) {
        enter_bootsel(c, "reboot to BOOTSEL");
        return;
    }
    if (type == 3) {
        region_t rg = {m, false, m->boot_param[0], m->boot_param[1]};
        m->boot_type = 3;
        if (try_launch_region(c, &rg, rg.base, false))
            return;
    }
    if (type == 0xd) {
        m->boot_type = 0xd;
        launch(c, c->vtor, m->boot_param[1], m->boot_param[0]);
        return;
    }
    m->boot_type = 0;
    region_t whole = {m, true, 0, m->flash_size};
    block_t image, ptb;
    memset(&image, 0, sizeof(image));
    memset(&ptb, 0, sizeof(ptb));
    bool found = find_blocks(&whole, &image, &ptb);
    if (found && ptb.partition_table)
        parse_partition_table(&whole, &ptb);
    if (found && image.image_def) {
        atrans_identity(m);
        if (try_launch_region(c, &whole, XIP_BASE, false))
            return;
    }
    if (found && ptb.partition_table) {
        for (int i = 0; i < pt.count; i++) {
            partition_t *p = &pt.p[i];
            if (p->flags & (1u << 9))
                continue;
            region_t rg = {m, true, part_start(p), part_end(p) - part_start(p)};
            if (try_launch_region(c, &rg, XIP_BASE, true)) {
                m->rom_partition = i;
                return;
            }
        }
    }
    enter_bootsel(c, "no bootable image in flash");
}

// ---------------------------------------------------------------------------------------------
// Core 1's wait for the launch handshake from core 0, as the ROM runs it.

enum { W_DRAIN, W_SEND, W_RECV };

// Core 1 echoes each word core 0 sends; a zero restarts the exchange, and the sequence
// 1, vector table, stack pointer, entry point launches it.
static uint32_t launch_seq[2][4];
static int send_next[2];

// What the ROM keeps between calls, which a new chip starts without.
static void forget_state(void)
{
    memset(&pt, 0, sizeof(pt));
    memset(launch_seq, 0, sizeof(launch_seq));
    memset(send_next, 0, sizeof(send_next));
}

// The ROM's FIFO accesses reach the bank its core's bus accesses do.
static bool fifo_push(chip_t *m, int core, uint32_t v)
{
    sio_fifo_t *tx = &m->sio[m->core[core].bus_ns].fifo[core];
    if (tx->count == 8)
        return false;
    tx->data[(tx->head + tx->count) & 7] = v;
    tx->count++;
    sio_fifo_irq(m);
    chip_sev(m, core);
    return true;
}

static bool fifo_pop(chip_t *m, int core, uint32_t *v)
{
    sio_fifo_t *rx = &m->sio[m->core[core].bus_ns].fifo[1 - core];
    if (!rx->count)
        return false;
    *v = rx->data[rx->head];
    rx->head = (rx->head + 1) & 7;
    rx->count--;
    sio_fifo_irq(m);
    return true;
}

static void core1_wait(cpu_t *c)
{
    chip_t *m = c->m;
    int id = c->id;
    for (;;) {
        if (c->hle_state == W_DRAIN) {
            uint32_t v;
            while (fifo_pop(m, id, &v))
                ;
            c->hle_val = 0;
            send_next[id] = W_RECV;
            c->hle_state = W_SEND;
        } else if (c->hle_state == W_SEND) {
            if (!fifo_push(m, id, c->hle_val)) {
                c->state = CPU_WFE;
                return;
            }
            c->hle_state = send_next[id];
            if (c->hle_state == W_RECV + 4) {
                c->vtor = launch_seq[id][1];
                c->msp = c->r[13] = launch_seq[id][2] & ~3u;
                c->hle_state = W_DRAIN;
                c->r[14] = stub_addr(HLE_DEAD) | 1;
                c->r[15] = launch_seq[id][3] & ~1u;
                return;
            }
        } else {
            int step = c->hle_state - W_RECV;
            uint32_t v;
            if (!fifo_pop(m, id, &v)) {
                c->state = CPU_WFE;
                return;
            }
            if (v == 0 || (step == 0 && v != 1)) {
                c->hle_state = W_DRAIN;
                continue;
            }
            launch_seq[id][step] = v;
            c->hle_val = v;
            send_next[id] = W_RECV + step + 1;
            c->hle_state = W_SEND;
        }
    }
}

void rom_core_start(cpu_t *c)
{
    c->r[15] = stub_addr(HLE_CORE_START);
    c->hle_state = W_DRAIN;
}

// ---------------------------------------------------------------------------------------------
// API functions

static uint64_t erase_time_ps(uint32_t len)
{
    return (uint64_t)(len / 0x1000) * 45ull * PS_PER_MS;
}

static int flash_op(cpu_t *c, uint32_t flags, uint32_t addr, uint32_t size, uint32_t buf)
{
    chip_t *m = c->m;
    uint32_t op = (flags >> 16) & 7;
    uint32_t storage = addr;
    if (flags & 1) {
        int rc = runtime_to_storage(m, addr, &storage);
        if (rc)
            return rc;
        uint32_t end;
        if (size && runtime_to_storage(m, addr + size - 1, &end))
            return -10;
    }
    if (storage + size > m->flash_size)
        return -10;
    switch (op) {
    case 0:
        if ((storage | size) & 0xfff)
            return -11;
        flash_erase(m, storage, size);
        charge(c, erase_time_ps(size));
        break;
    case 1:
        if ((storage | size) & 0xff)
            return -11;
        for (uint32_t i = 0; i < size; i++) {
            uint8_t b = mr8(m, buf + i);
            flash_program(m, storage + i, &b, 1);
        }
        charge(c, (uint64_t)(size / 256) * 400ull * PS_PER_US);
        break;
    case 2:
        for (uint32_t i = 0; i < size; i++)
            mw8(m, buf + i, m->flash[storage + i]);
        break;
    default:
        return -5;
    }
    xip_flush(m);
    return 0;
}

static int sys_info(cpu_t *c, uint32_t out, uint32_t words, uint32_t flags)
{
    chip_t *m = c->m;
    uint32_t data[32];
    int n = 1;
    uint32_t supported = 0;
    if (flags & 0x1) {
        supported |= 1;
        data[n++] = m->board->num_gpio > 30 ? 0 : 1;
        data[n++] = 0xa3d4c6e1u;
        data[n++] = 0x5b7f02c9u;
    }
    if (flags & 0x2) {
        supported |= 2;
        data[n++] = 0;
    }
    if (flags & 0x4) {
        supported |= 4;
        data[n++] = 0;
    }
    if (flags & 0x8) {
        supported |= 8;
        data[n++] = m->bootram[DEVINFO_ADDR - 0x400e0000u] | (m->bootram[DEVINFO_ADDR - 0x400e0000u + 1] << 8);
    }
    if (flags & 0x10) {
        supported |= 0x10;
        for (int i = 0; i < 4; i++)
            data[n++] = 0x9e3779b9u * (uint32_t)(i + 1) ^ (uint32_t)m->boot_count;
    }
    if (flags & 0x20) {
        supported |= 0x20;
        data[n++] = 0x12345678u;
        data[n++] = 0x9abcdef0u;
    }
    if (flags & 0x40) {
        supported |= 0x40;
        uint8_t part = (uint8_t)(int8_t)m->rom_partition;
        data[n++] = 0xff | ((m->boot_type & 0xff) << 8) | ((uint32_t)part << 16);
        data[n++] = 0;
        data[n++] = m->boot_param[0];
        data[n++] = m->boot_param[1];
    }
    data[0] = supported;
    if ((uint32_t)n > words)
        return -13;
    for (int i = 0; i < n; i++)
        mw32(m, out + 4u * (uint32_t)i, data[i]);
    return n;
}

static int pt_info(cpu_t *c, uint32_t out, uint32_t words, uint32_t pf)
{
    chip_t *m = c->m;
    if (!pt.loaded)
        return -12;
    uint32_t flags = pf & 0xffff;
    uint32_t data[256];
    int n = 1;
    uint32_t supported = flags & (0x1 | 0x10 | 0x20 | 0x40 | 0x80 | 0x8000);
    if (flags & 1) {
        data[n++] = (uint32_t)pt.count | (pt.present ? 0x100u : 0);
        data[n++] = pt.unpartitioned_flags;
    }
    int first = 0, last = pt.count - 1;
    if (flags & 0x8000) {
        first = last = (int)(pf >> 24);
        if (first >= pt.count)
            return -17;
    }
    for (int i = first; i <= last && n < 240; i++) {
        partition_t *p = &pt.p[i];
        if (flags & 0x10) {
            data[n++] = p->loc;
            data[n++] = p->flags;
        }
        if ((flags & 0x20) && (p->flags & 1)) {
            data[n++] = p->id[0];
            data[n++] = p->id[1];
        }
        if (flags & 0x40) {
            for (int k = 0; k < p->nfamilies; k++)
                data[n++] = p->families[k];
        }
        if ((flags & 0x80) && (p->flags & (1u << 12))) {
            size_t len = strlen(p->name);
            uint8_t bytes[68] = {0};
            bytes[0] = (uint8_t)len;
            memcpy(bytes + 1, p->name, len);
            for (size_t k = 0; k < (len + 4) / 4; k++)
                data[n++] = rd32le(bytes + 4 * k);
        }
    }
    data[0] = supported;
    if ((uint32_t)n > words)
        return -13;
    for (int i = 0; i < n; i++)
        mw32(m, out + 4u * (uint32_t)i, data[i]);
    return n;
}

static int chain_image(cpu_t *c, uint32_t base, uint32_t size)
{
    chip_t *m = c->m;
    region_t rg;
    bool flash = (base >> 28) == 1;
    if (flash) {
        uint32_t storage;
        if ((int32_t)base < 0) {
            storage = base & 0x7fffffffu;
        } else if (runtime_to_storage(m, base, &storage) != 0) {
            return -10;
        }
        rg = (region_t){m, true, storage, size};
    } else {
        rg = (region_t){m, false, base, size};
    }
    block_t image, ptb;
    memset(&image, 0, sizeof(image));
    if (!find_blocks(&rg, &image, &ptb) || !image.image_def)
        return -16;
    m->boot_type |= 0x80;
    for (int i = 0; i < pt.count; i++)
        if (flash && part_start(&pt.p[i]) == rg.base)
            m->rom_partition = i;
    try_launch_region(c, &rg, flash ? XIP_BASE : base, flash);
    return 1;
}

static void schedule_reboot(chip_t *m, uint32_t delay_ms);

static int reboot(cpu_t *c, uint32_t flags, uint32_t delay_ms, uint32_t p0, uint32_t p1)
{
    chip_t *m = c->m;
    uint32_t type = flags & 0xf;
    if (flags & 0x20) {
        if (type == 2)
            type = 2;
    }
    m->boot_type = type;
    m->boot_param[0] = p0;
    m->boot_param[1] = p1;
    m->wd_scratch[4] = 0;
    schedule_reboot(m, delay_ms);
    if (flags & 0x100) {
        c->state = CPU_WFE;
        c->r[15] = stub_addr(HLE_DEAD);
        return 1;
    }
    return 0;
}

static emu_event_t reboot_ev;

static void reboot_fire(chip_t *m, emu_event_t *ev)
{
    (void)ev;
    m->wd_reason = 1;
    chip_request(m, REQ_RESET, "rom reboot");
}

static void schedule_reboot(chip_t *m, uint32_t delay_ms)
{
    reboot_ev.fn = reboot_fire;
    ev_schedule(m, &reboot_ev, m->now + (uint64_t)(delay_ms ? delay_ms : 1) * PS_PER_MS);
}

static int otp_access(cpu_t *c, uint32_t buf, uint32_t len, uint32_t cmd)
{
    chip_t *m = c->m;
    if (cmd & (1u << 16))
        return 0;
    for (uint32_t i = 0; i < len; i++)
        mw8(m, buf + i, 0);
    return 0;
}

static uint32_t table_lookup(cpu_t *c, uint32_t code, uint32_t mask)
{
    chip_t *m = c->m;
    if (mask & 0x40) {
        switch (code) {
        case CODE('G', 'R'): return ROM_DATA;
        case CODE('F', 'D'): return ROM_DATA + 8;
        case CODE('P', 'T'): return RESIDENT_PT_ADDR;
        case CODE('X', 'F'): return ROM_DATA + 4;
        default: break;
        }
    }
    if (mask & 0x14) {
        for (size_t i = 0; i < sizeof(func_table) / sizeof(func_table[0]); i++)
            if (func_table[i].code == code)
                return stub_addr(func_table[i].hle) | 1;
    }
    if (m->trace)
        emu_log("rom: lookup of unknown code %c%c (mask %x)", code & 0xff, (code >> 8) & 0xff, mask);
    return 0;
}

void rom_hle(cpu_t *c, unsigned n)
{
    chip_t *m = c->m;
    uint32_t *r = c->r;
    switch (n) {
    case HLE_CORE_START:
        if (c->id == 0) {
            boot_core0(c);
        } else {
            c->r[15] = stub_addr(HLE_CORE_START);
            core1_wait(c);
        }
        return;
    case HLE_DEAD:
        c->r[15] = stub_addr(HLE_DEAD);
        c->state = CPU_WFE;
        return;
    case HLE_TABLE_LOOKUP:
        r[0] = table_lookup(c, r[0], r[1]);
        break;
    case HLE_FLASH_ENTER_CMD_XIP:
    case HLE_FLASH_EXIT_XIP:
    case HLE_CONNECT_INTERNAL_FLASH:
    case HLE_FLASH_SELECT_XIP_READ_MODE:
        break;
    case HLE_FLASH_FLUSH_CACHE:
        xip_flush(m);
        break;
    case HLE_FLASH_RANGE_ERASE:
        flash_erase(m, r[0] & 0x00ffffffu, r[1]);
        charge(c, erase_time_ps(r[1]));
        xip_flush(m);
        break;
    case HLE_FLASH_RANGE_PROGRAM: {
        uint32_t off = r[0] & 0x00ffffffu, src = r[1], len = r[2];
        for (uint32_t i = 0; i < len; i++) {
            uint8_t b = mr8(m, src + i);
            flash_program(m, off + i, &b, 1);
        }
        charge(c, (uint64_t)((len + 255) / 256) * 400ull * PS_PER_US);
        xip_flush(m);
        break;
    }
    case HLE_FLASH_OP:
        r[0] = (uint32_t)flash_op(c, r[0], r[1], r[2], r[3]);
        break;
    case HLE_FLASH_RUNTIME_TO_STORAGE: {
        uint32_t st;
        int rc = runtime_to_storage(m, r[0], &st);
        r[0] = rc ? (uint32_t)rc : st;
        break;
    }
    case HLE_GET_SYS_INFO:
        r[0] = (uint32_t)sys_info(c, r[0], r[1], r[2]);
        break;
    case HLE_GET_PARTITION_TABLE_INFO: {
        uint32_t flags = r[2];
        if (!pt.loaded)
            load_pt_from_flash(m);
        r[0] = (uint32_t)pt_info(c, r[0], r[1], r[2]);
        if (m->trace)
            emu_log("rom: get_partition_table_info(%08x) -> %d (%d partitions)", flags, (int)r[0], pt.count);
        break;
    }
    case HLE_LOAD_PARTITION_TABLE:
        load_pt_from_flash(m);
        if (m->trace)
            emu_log("rom: load_partition_table -> %d partitions%s", pt.count, pt.present ? "" : " (none present)");
        r[0] = 0;
        break;
    case HLE_PICK_AB_PARTITION:
        r[0] = r[2] & 0xff;
        break;
    case HLE_GET_B_PARTITION:
        r[0] = (uint32_t)-17;
        break;
    case HLE_GET_UF2_TARGET_PARTITION:
        r[0] = (uint32_t)-17;
        for (int i = 0; i < pt.count; i++)
            if (pt.p[i].flags & (7u << 17)) {
                r[0] = (uint32_t)i;
                break;
            }
        break;
    case HLE_CHAIN_IMAGE: {
        uint32_t pc = r[15];
        int rc = chain_image(c, r[2], r[3]);
        if (rc != 1) {
            r[0] = (uint32_t)rc;
            r[15] = pc;
            break;
        }
        return;
    }
    case HLE_EXPLICIT_BUY:
    case HLE_SET_NS_API_PERMISSION:
    case HLE_BOOTROM_STATE_RESET:
    case HLE_SET_BOOTROM_STACK:
        r[0] = 0;
        break;
    case HLE_FLASH_RESET_ADDRESS_TRANS:
        atrans_identity(m);
        break;
    case HLE_REBOOT:
        if (reboot(c, r[0], r[1], r[2], r[3]) == 1)
            return;
        r[0] = 0;
        break;
    case HLE_SET_ROM_CALLBACK:
        r[0] = 0;
        break;
    case HLE_SECURE_CALL:
        r[0] = (uint32_t)-4;
        break;
    case HLE_VALIDATE_NS_BUFFER:
        r[0] = r[0];
        break;
    case HLE_OTP_ACCESS:
        r[0] = (uint32_t)otp_access(c, r[0], r[1], r[2]);
        break;
    default:
        emu_log("rom: unimplemented function %u called from %08x", n, r[14]);
        r[0] = (uint32_t)-4;
        break;
    }
    // The stub's following BX LR returns to the caller.
    c->cyc += 8;
}
