#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chip.h"

chip_t *g_chip;

const board_info_t board_table[] = {
    {BOARD_PICO2, "pico2", "pico2", 4u << 20, 0, 30, -1, false, false},
    {BOARD_PICO2_W, "pico2_w", "pico2_w", 4u << 20, 0, 30, -1, true, false},
    {BOARD_PIMORONI_PICO_PLUS2_W, "pimoroni_pico_plus2_w", "pimoroni_pico_plus2_w_rp2350", 16u << 20, 8u << 20, 48,
     47, true, false},
    {BOARD_PIMORONI_PICO_PLUS2, "pimoroni_pico_plus2", "pimoroni_pico_plus2_rp2350", 16u << 20, 8u << 20, 48, 47,
     false, false},
    {BOARD_PICO, "pico", "pico", 2u << 20, 0, 30, -1, false, true},
    {BOARD_PICO_W, "pico_w", "pico_w", 2u << 20, 0, 30, -1, true, true},
    {0, NULL, NULL, 0, 0, 0, 0, false, false},
};

const board_info_t *board_find(const char *name)
{
    for (const board_info_t *b = board_table; b->name; b++)
        if (!strcmp(name, b->name) || !strcmp(name, b->sdk_name))
            return b;
    if (!strcmp(name, "pim2w") || !strcmp(name, "pico_plus2_w"))
        return &board_table[2];
    if (!strcmp(name, "pico2w"))
        return &board_table[1];
    if (!strcmp(name, "picow"))
        return board_find("pico_w");
    return NULL;
}

void ev_schedule(chip_t *m, emu_event_t *ev, uint64_t when)
{
    // Already where taking it out and putting it back would leave it.
    if (ev->queued && ev->when == when && (!ev->next || ev->next->when > when))
        return;
    if (ev->queued)
        ev_cancel(m, ev);
    ev->when = when;
    emu_event_t **p = &m->events;
    while (*p && (*p)->when <= when)
        p = &(*p)->next;
    ev->next = *p;
    *p = ev;
    ev->queued = true;
}

void ev_cancel(chip_t *m, emu_event_t *ev)
{
    if (!ev->queued)
        return;
    for (emu_event_t **p = &m->events; *p; p = &(*p)->next) {
        if (*p == ev) {
            *p = ev->next;
            break;
        }
    }
    ev->queued = false;
    ev->next = NULL;
}

static uint64_t ext_bit(chip_t *m, unsigned irq, int core)
{
    if (!m->rp2040)
        return 1ull << irq;
    int line = rp2040_irq_line(irq, core);
    return line < 0 ? 0 : 1ull << line;
}

void chip_irq(chip_t *m, unsigned irq, bool level)
{
    uint64_t bit = 1ull << irq;
    uint64_t old = m->irq_lines;
    if (level)
        m->irq_lines |= bit;
    else
        m->irq_lines &= ~bit;
    uint64_t e = ext_bit(m, irq, 0);
    if (level)
        m->irq_ext |= e;
    else
        m->irq_ext &= ~e;
    if (level && !(old & bit))
        for (int i = 0; i < 2; i++)
            cpu_poke(&m->core[i]);
}

void chip_irq_core(chip_t *m, int core, unsigned irq, bool level)
{
    uint64_t bit = 1ull << irq;
    uint64_t old = m->irq_core[core];
    if (level)
        m->irq_core[core] |= bit;
    else
        m->irq_core[core] &= ~bit;
    uint64_t e = ext_bit(m, irq, core);
    if (level)
        m->irq_core_ext[core] |= e;
    else
        m->irq_core_ext[core] &= ~e;
    if (level && !(old & bit))
        cpu_poke(&m->core[core]);
}

void chip_sev(chip_t *m, int from_core)
{
    for (int i = 0; i < 2; i++) {
        cpu_t *c = &m->core[i];
        if (c->state == CPU_OFF || c->state == CPU_LOCKUP)
            continue;
        if (c->state == CPU_WFE && from_core >= 0 && c->t < m->core[from_core].t)
            c->t = m->core[from_core].t;
        if (i == from_core)
            c->event = true;
        else
            cpu_wake(c, true);
    }
}

void chip_request(chip_t *m, int what, const char *reason)
{
    pthread_mutex_lock(&m->power_lock);
    if (what == REQ_RESET)
        m->req_reset = true;
    else if (what == REQ_BOOTSEL)
        m->req_bootsel = true;
    else
        m->req_poweroff = true;
    snprintf(m->req_reason, sizeof(m->req_reason), "%s", reason);
    atomic_store(&m->req_any, true);
    pthread_mutex_unlock(&m->power_lock);
}

void chip_dreq(chip_t *m, unsigned dreq, bool level)
{
    if (dreq >= DREQ_COUNT)
        return;
    bool old = m->dreq[dreq];
    m->dreq[dreq] = level;
    if (level && !old)
        dma_kick(m);
}

// ATRANS windows map 4 MB of address space each onto the device behind a chip select, in 4 KB
// units; the untranslated aliases bypass them.
uint8_t *xip_translate(chip_t *m, uint32_t addr)
{
    if (m->rp2040)
        return rp2040_xip_translate(m, addr);
    unsigned alias = (addr >> 24) & 0x1f;
    bool cs1 = alias & 1;
    bool translate = alias < 0x1c;
    uint32_t off = addr & 0x00ffffffu;
    if (translate) {
        unsigned w = (cs1 ? 4 : 0) + (off >> 22);
        uint32_t at = m->qmi.atrans[w];
        uint32_t size = ((at >> 16) & 0x7ff) << 12;
        uint32_t base = (at & 0xfff) << 12;
        uint32_t woff = off & 0x3fffff;
        if (woff >= size)
            return NULL;
        off = base + woff;
    }
    if (cs1) {
        // Address bits above the device's size are ignored, which is how firmware sizes PSRAM.
        if (!m->psram)
            return NULL;
        return m->psram + (off & (m->psram_size - 1));
    }
    return m->flash + (off & (m->flash_size - 1));
}

void chip_rebuild_pages(chip_t *m)
{
    memset(m->rpage, 0, sizeof(m->rpage));
    memset(m->wpage, 0, sizeof(m->wpage));
    m->rpage[0] = m->rom;
    // The RP2040's last SRAM page holds only the two 4 KB banks; the rest of it is mapped too,
    // which no firmware reaches.
    uint32_t sram_pages = (m->sram_size + 0xffff) >> 16;
    for (uint32_t p = 0; p < sram_pages; p++) {
        m->rpage[0x2000 + p] = m->sram + p * 0x10000;
        m->wpage[0x2000 + p] = m->sram + p * 0x10000;
    }
    static const uint32_t aliases[] = {0x10, 0x11, 0x14, 0x15, 0x1c, 0x1d};
    static const uint32_t aliases_rp2040[] = {0x10, 0x11, 0x12, 0x13};
    const uint32_t *al = m->rp2040 ? aliases_rp2040 : aliases;
    unsigned nal = m->rp2040 ? 4 : sizeof(aliases) / sizeof(aliases[0]);
    for (unsigned i = 0; i < nal; i++) {
        uint32_t alias = al[i];
        for (uint32_t p = 0; p < 0x100; p++) {
            uint32_t addr = (alias << 24) | (p << 16);
            uint8_t *ptr = xip_translate(m, addr);
            if (!ptr)
                continue;
            // A page that would run off the end of the device must take the slow path.
            if (!xip_translate(m, addr + 0xffff) || xip_translate(m, addr + 0xffff) != ptr + 0xffff)
                continue;
            m->rpage[(addr >> 16)] = ptr;
            if ((alias & 1) && !m->rp2040)
                m->wpage[(addr >> 16)] = ptr;
        }
    }
    memcpy(m->rpage_plain, m->rpage, sizeof(m->rpage_plain));
    memset(m->rpage_plain + 0x1000, 0, 0x1000 * sizeof(m->rpage_plain[0]));
    // A core part-way through a predecoded block must look at memory again before going on.
    m->core[0].check_exc = m->core[1].check_exc = true;
    m->code_gen++;
}

uint32_t xip_access(chip_t *m, uint32_t addr)
{
    unsigned alias = (addr >> 24) & 0x1f;
    uint32_t miss;
    if (m->rp2040) {
        miss = m->rp.ssi.miss_cyc;
    } else {
        uint32_t clkdiv = m->qmi.m[alias & 1][0] & 0xff;
        if (!clkdiv)
            clkdiv = 256;
        miss = 8 + 24 * clkdiv;
    }
    if (alias >= m->xip_cached_top || !(m->xip_ctrl & 1))
        return miss;
    // The RP2040's aliases all reach the same flash; the RP2350's bit 24 picks the chip select.
    uint32_t line = (addr & (m->rp2040 ? 0x00ffffffu : 0x01ffffffu)) >> 3;
    uint32_t set = line & 1023;
    uint32_t tag = (line >> 10) | 0x80000000u;
    m->xip_ctr_acc++;
    m->xip_mru[set] = tag;
    if (m->xip_tag[set][0] == tag) {
        m->xip_lru[set] = 1;
        m->xip_ctr_hit++;
        return 0;
    }
    if (m->xip_tag[set][1] == tag) {
        m->xip_lru[set] = 0;
        m->xip_ctr_hit++;
        return 0;
    }
    if (m->rp2040 && alias == 0x11)
        return miss;
    int way = m->xip_lru[set];
    m->xip_tag[set][way] = tag;
    m->xip_lru[set] = (uint8_t)!way;
    return miss;
}

void xip_flush(chip_t *m)
{
    memset(m->xip_tag, 0, sizeof(m->xip_tag));
    memset(m->xip_mru, 0, sizeof(m->xip_mru));
    m->core[0].fetch_line = m->core[1].fetch_line = 1;
    // Predecoded blocks assume the fetch line only changes as they fetch.
    m->core[0].check_exc = m->core[1].check_exc = true;
}

static uint32_t extract(uint32_t word, uint32_t addr, int size)
{
    if (size == 4)
        return word;
    unsigned sh = (addr & 3) * 8;
    return size == 2 ? (word >> sh) & 0xffff : (word >> sh) & 0xff;
}

static uint32_t replicate(uint32_t v, int size)
{
    if (size == 1)
        return (v & 0xff) * 0x01010101u;
    if (size == 2)
        return (v & 0xffff) * 0x00010001u;
    return v;
}

static uint32_t otp_data_read(chip_t *m, uint32_t addr);

static uint32_t apb_reg_read(chip_t *m, uint32_t addr, bool peek)
{
    uint32_t block = addr & 0xffff8000u;
    uint32_t off = addr & 0xfff;
    switch (block) {
    case 0x40028000: return iobank_read(m, off);
    case 0x40030000: return ioqspi_read(m, off);
    case 0x40038000: return pads_read(m, false, off);
    case 0x40040000: return pads_read(m, true, off);
    case 0x40070000: return pl011_read(m, &m->uart[0], off, peek);
    case 0x40078000: return pl011_read(m, &m->uart[1], off, peek);
    case 0x40080000: return pl022_read(m, &m->spi[0], off, peek);
    case 0x40088000: return pl022_read(m, &m->spi[1], off, peek);
    case 0x40090000: return dw_i2c_read(m, &m->i2c[0], off, peek);
    case 0x40098000: return dw_i2c_read(m, &m->i2c[1], off, peek);
    case 0x400a8000: return pwm_read(m, off);
    case 0x400b0000: return timer_read(m, &m->timer[0], off, peek);
    case 0x400b8000: return timer_read(m, &m->timer[1], off, peek);
    case 0x400c8000: return xipctrl_read(m, off);
    case 0x400d0000: return qmi_read(m, off);
    case 0x400d8000: return watchdog_read(m, off);
    case 0x40100000: return powman_read(m, off);
    default: return sysctl_read(m, block, off);
    }
}

static void apb_reg_write(chip_t *m, uint32_t addr, uint32_t v)
{
    uint32_t block = addr & 0xffff8000u;
    uint32_t off = addr & 0xfff;
    switch (block) {
    case 0x40028000: iobank_write(m, off, v); break;
    case 0x40030000: ioqspi_write(m, off, v); break;
    case 0x40038000: pads_write(m, false, off, v); break;
    case 0x40040000: pads_write(m, true, off, v); break;
    case 0x40070000: pl011_write(m, &m->uart[0], off, v); break;
    case 0x40078000: pl011_write(m, &m->uart[1], off, v); break;
    case 0x40080000: pl022_write(m, &m->spi[0], off, v); break;
    case 0x40088000: pl022_write(m, &m->spi[1], off, v); break;
    case 0x40090000: dw_i2c_write(m, &m->i2c[0], off, v); break;
    case 0x40098000: dw_i2c_write(m, &m->i2c[1], off, v); break;
    case 0x400a8000: pwm_write(m, off, v); break;
    case 0x400b0000: timer_write(m, &m->timer[0], off, v); break;
    case 0x400b8000: timer_write(m, &m->timer[1], off, v); break;
    case 0x400c8000: xipctrl_write(m, off, v); break;
    case 0x400d0000: qmi_write(m, off, v); break;
    case 0x400d8000: watchdog_write(m, off, v); break;
    case 0x40100000: powman_write(m, off, v); break;
    default: sysctl_write(m, block, off, v); break;
    }
}

static uint32_t ahb_reg_read(chip_t *m, uint32_t addr, bool peek, bool *ok)
{
    uint32_t off = addr & 0xfff;
    switch (addr & 0xfff00000u) {
    case 0x50000000: return dma_read(m, off, peek);
    case 0x50100000:
        if ((addr & 0xf0000) == 0x10000)
            return m->usb_regs[(off >> 2) & 63];
        break;
    case 0x50200000: return pio_read(m, &m->pio[0], off, peek);
    case 0x50300000: return pio_read(m, &m->pio[1], off, peek);
    case 0x50400000: return pio_read(m, &m->pio[2], off, peek);
    case 0x50500000:
    case 0x50600000: return 0;
    default: break;
    }
    *ok = false;
    return 0;
}

static void ahb_reg_write(chip_t *m, uint32_t addr, uint32_t v, bool *ok)
{
    uint32_t off = addr & 0xfff;
    switch (addr & 0xfff00000u) {
    case 0x50000000: dma_write(m, off, v); return;
    case 0x50100000:
        if ((addr & 0xf0000) == 0x10000) {
            m->usb_regs[(off >> 2) & 63] = v;
            return;
        }
        break;
    case 0x50200000: pio_write(m, &m->pio[0], off, v); return;
    case 0x50300000: pio_write(m, &m->pio[1], off, v); return;
    case 0x50400000: pio_write(m, &m->pio[2], off, v); return;
    case 0x50500000:
    case 0x50600000: return;
    default: break;
    }
    *ok = false;
}

static uint32_t periph_read(chip_t *m, uint32_t addr, bool *ok)
{
    if (m->rp2040)
        return rp2040_periph_read(m, addr & ~0x3003u, false, ok);
    if (addr >= 0x40130000u && addr < 0x40140000u)
        return otp_data_read(m, addr);
    uint32_t a = addr & ~0x3003u;
    if ((addr >> 28) == 4)
        return apb_reg_read(m, a, false);
    return ahb_reg_read(m, a, false, ok);
}

static void periph_write(chip_t *m, uint32_t addr, uint32_t v, bool *ok)
{
    if (!m->rp2040 && addr >= 0x40130000u && addr < 0x40140000u)
        return;
    uint32_t a = addr & ~0x3003u;
    unsigned alias = (addr >> 12) & 3;
    bool apb = (addr >> 28) == 4;
    if (alias) {
        bool pok = true;
        uint32_t old = m->rp2040 ? rp2040_periph_read(m, a, true, &pok)
                       : apb     ? apb_reg_read(m, a, true)
                                 : ahb_reg_read(m, a, true, &pok);
        if (alias == 1)
            v = old ^ v;
        else if (alias == 2)
            v = old | v;
        else
            v = old & ~v;
    }
    if (m->rp2040)
        rp2040_periph_write(m, a, v, ok);
    else if (apb)
        apb_reg_write(m, a, v);
    else
        ahb_reg_write(m, a, v, ok);
}

// The OTP holds factory data the SDK reads: the chip ID rows and the boot flags. Everything else
// reads as unprogrammed.
static uint32_t otp_data_read(chip_t *m, uint32_t addr)
{
    (void)m;
    bool raw = (addr & 0x4000) != 0;
    uint32_t row = raw ? (addr & 0x3fff) >> 2 : (addr & 0x3fff) >> 1;
    static const uint16_t chipid[4] = {0x1d5a, 0x8f3b, 0x2c41, 0xe350};
    uint32_t v = 0;
    if (row < 4)
        v = chipid[row];
    else if (row == 4)
        v = 0x1234;
    if (raw)
        return v;
    uint32_t v2 = 0;
    uint32_t row2 = row + 1;
    if (row2 < 4)
        v2 = chipid[row2];
    else if (row2 == 4)
        v2 = 0x1234;
    return (addr & 2) ? v : (v | (v2 << 16));
}

// SRAM banks 0-3 one after another at 0x21000000 on the RP2040, rather than striped word by word.
static uint32_t *unstriped(chip_t *m, uint32_t addr)
{
    uint32_t o = addr & 0xffffff;
    if (!m->rp2040 || (addr >> 24) != 0x21 || o >= 0x40000)
        return NULL;
    uint32_t word = (o & 0xffff) >> 2, bank = o >> 16;
    return (uint32_t *)(m->sram + ((word * 4 + bank) << 2));
}

static uint32_t mem_read(chip_t *m, cpu_t *c, uint32_t addr, int size, bool *ok)
{
    switch (addr >> 28) {
    case 0x0:
        if (addr < ROM_SIZE)
            return size == 4 ? rd32le(m->rom + addr) : size == 2 ? rd16le(m->rom + addr) : m->rom[addr];
        break;
    case 0x1: {
        if (m->rp2040)
            return rp2040_xip_read(m, c, addr, size, ok);
        if (addr >= XIP_SRAM_BASE && addr < XIP_NOCACHE_BASE) {
            uint32_t o = addr & 0x3fff;
            return size == 4 ? rd32le(m->xip_sram + o) : size == 2 ? rd16le(m->xip_sram + o) : m->xip_sram[o];
        }
        if ((addr >> 26) == 0x6)
            return 0;
        uint8_t *p = xip_translate(m, addr);
        if (p) {
            if (c)
                c->cyc += xip_cost(m, addr);
            return size == 4 ? rd32le(p) : size == 2 ? rd16le(p) : *p;
        }
        if (((addr >> 24) & 1) && !m->psram)
            return size == 4 ? 0xffffffffu : size == 2 ? 0xffffu : 0xffu;
        break;
    }
    case 0x2: {
        if (addr - SRAM_BASE < m->sram_size) {
            uint32_t o = addr - SRAM_BASE;
            return size == 4 ? rd32le(m->sram + o) : size == 2 ? rd16le(m->sram + o) : m->sram[o];
        }
        uint32_t *w = unstriped(m, addr);
        if (w)
            return extract(*w, addr, size);
        break;
    }
    case 0x4:
        if (!m->rp2040 && (addr & 0xffffc000u) == 0x400e0000u && (addr & 0xfff) < 0x400) {
            uint32_t o = addr & 0x3ff;
            return size == 4 ? rd32le(m->bootram + o) : size == 2 ? rd16le(m->bootram + o) : m->bootram[o];
        }
        if (c)
            c->cyc += 2;
        return extract(periph_read(m, addr, ok), addr, size);
    case 0x5:
        if ((addr & 0xfff00000u) == USB_DPRAM_BASE && (addr & 0xfffff) < USB_DPRAM_SIZE) {
            uint32_t o = addr & 0xfff;
            return size == 4 ? rd32le(m->usb_dpram + o) : size == 2 ? rd16le(m->usb_dpram + o) : m->usb_dpram[o];
        }
        if (c)
            c->cyc += 1;
        return extract(periph_read(m, addr, ok), addr, size);
    case 0xd:
        if (c)
            return extract(sio_read(c, addr & 0x1ffff), addr, size);
        break;
    case 0xe:
        if (c) {
            if (addr < 0xe0040000u)
                return extract(cpu_ppb_read(c, addr & 0xffff), addr, size);
            return 0;
        }
        break;
    default:
        break;
    }
    *ok = false;
    return 0;
}

static void mem_write(chip_t *m, cpu_t *c, uint32_t addr, uint32_t v, int size, bool *ok)
{
    switch (addr >> 28) {
    case 0x0:
        return;
    case 0x1: {
        if (m->rp2040) {
            rp2040_xip_write(m, addr, v, size, ok);
            return;
        }
        if (addr >= XIP_SRAM_BASE && addr < XIP_NOCACHE_BASE) {
            uint32_t o = addr & 0x3fff;
            if (size == 4)
                wr32le(m->xip_sram + o, v);
            else if (size == 2)
                wr16le(m->xip_sram + o, (uint16_t)v);
            else
                m->xip_sram[o] = (uint8_t)v;
            return;
        }
        if ((addr >> 26) == 0x6) {
            // Cache maintenance writes: invalidating by set/way or address is all that matters here.
            if (((addr >> 24) & 3) == 0)
                xip_flush(m);
            return;
        }
        uint8_t *p = xip_translate(m, addr);
        if (p && (addr >> 24) & 1) {
            if (size == 4)
                wr32le(p, v);
            else if (size == 2)
                wr16le(p, (uint16_t)v);
            else
                *p = (uint8_t)v;
            return;
        }
        if (p || (((addr >> 24) & 1) && !m->psram))
            return;
        break;
    }
    case 0x2: {
        if (addr - SRAM_BASE < m->sram_size) {
            uint32_t o = addr - SRAM_BASE;
            if (size == 4)
                wr32le(m->sram + o, v);
            else if (size == 2)
                wr16le(m->sram + o, (uint16_t)v);
            else
                m->sram[o] = (uint8_t)v;
            return;
        }
        uint32_t *w = unstriped(m, addr);
        if (w) {
            uint8_t *b = (uint8_t *)w + (addr & 3);
            if (size == 4)
                wr32le(b, v);
            else if (size == 2)
                wr16le(b, (uint16_t)v);
            else
                *b = (uint8_t)v;
            return;
        }
        break;
    }
    case 0x4:
        if (!m->rp2040 && (addr & 0xffffc000u) == 0x400e0000u && (addr & 0xfff) < 0x400) {
            // Boot RAM sits on APB and so has the atomic XOR, set and clear aliases.
            uint32_t o = addr & 0x3ff;
            uint32_t old = size == 4 ? rd32le(m->bootram + o) : size == 2 ? rd16le(m->bootram + o) : m->bootram[o];
            switch ((addr >> 12) & 3) {
            case 1: v = old ^ v; break;
            case 2: v = old | v; break;
            case 3: v = old & ~v; break;
            default: break;
            }
            if (size == 4)
                wr32le(m->bootram + o, v);
            else if (size == 2)
                wr16le(m->bootram + o, (uint16_t)v);
            else
                m->bootram[o] = (uint8_t)v;
            return;
        }
        if (c)
            c->cyc += 2;
        periph_write(m, addr, replicate(v, size), ok);
        return;
    case 0x5:
        if ((addr & 0xfff00000u) == USB_DPRAM_BASE && (addr & 0xfffff) < USB_DPRAM_SIZE) {
            uint32_t o = addr & 0xfff;
            if (size == 4)
                wr32le(m->usb_dpram + o, v);
            else if (size == 2)
                wr16le(m->usb_dpram + o, (uint16_t)v);
            else
                m->usb_dpram[o] = (uint8_t)v;
            return;
        }
        if (c)
            c->cyc += 1;
        periph_write(m, addr, replicate(v, size), ok);
        return;
    case 0xd:
        if (c) {
            sio_write(c, addr & 0x1ffff, replicate(v, size));
            return;
        }
        break;
    case 0xe:
        if (c) {
            if (addr < 0xe0040000u)
                cpu_ppb_write(c, addr & 0xffff, v);
            return;
        }
        break;
    default:
        break;
    }
    *ok = false;
}

static inline void touch_now(chip_t *m, cpu_t *c)
{
    if (c->t > m->now)
        m->now = c->t;
}

static __attribute__((noinline)) uint32_t bus_read_decode(cpu_t *c, uint32_t addr, int size)
{
    chip_t *m = c->m;
    bool ok = true;
    uint32_t v = mem_read(m, c, addr, size, &ok);
    if (!ok) {
        if (m->trace)
            emu_log("core %d bus error reading %08x (pc %08x)", c->id, addr, c->r[15]);
        c->fault = true;
        c->check_exc = true;
        c->cfsr |= (1u << 9) | (1u << 15);
        c->bfar = addr;
    }
    return v;
}

uint32_t bus_read(cpu_t *c, uint32_t addr, int size)
{
    chip_t *m = c->m;
    touch_now(m, c);
    // The SDK reads the timers constantly (time_us_64, busy_wait), so they skip the general
    // decode, which would arrive at the same call, and its stack frame.
    if ((addr & 0xffff0000u) == 0x400b0000u) {
        c->cyc += 2;
        return extract(timer_read(m, &m->timer[(addr >> 15) & 1], addr & 0xffc, false), addr, size);
    }
    return bus_read_decode(c, addr, size);
}

void bus_write(cpu_t *c, uint32_t addr, uint32_t v, int size)
{
    chip_t *m = c->m;
    touch_now(m, c);
    bool ok = true;
    mem_write(m, c, addr, v, size, &ok);
    if (!ok) {
        if (m->trace)
            emu_log("core %d bus error writing %08x (pc %08x)", c->id, addr, c->r[15]);
        c->fault = true;
        c->check_exc = true;
        c->cfsr |= (1u << 9) | (1u << 15);
        c->bfar = addr;
    }
}

uint16_t bus_fetch16_slow(cpu_t *c, uint32_t addr)
{
    chip_t *m = c->m;
    bool ok = true;
    if ((addr >> 28) == 1 || (addr >> 28) == 2) {
        uint32_t v = mem_read(m, c, addr, 2, &ok);
        if (ok)
            return (uint16_t)v;
    }
    if ((addr & 0xfff00000u) == USB_DPRAM_BASE && (addr & 0xfffff) < USB_DPRAM_SIZE)
        return rd16le(m->usb_dpram + (addr & 0xfff));
    if ((addr & 0xffffc000u) == 0x400e0000u && (addr & 0xfff) < 0x400)
        return rd16le(m->bootram + (addr & 0x3ff));
    if (m->trace)
        emu_log("core %d instruction fetch from %08x", c->id, addr);
    c->fault = true;
    c->cfsr |= 1u << 8;
    return 0xbf00;
}

uint32_t sys_read(chip_t *m, uint32_t addr, int size, bool *ok)
{
    bool k = true;
    uint32_t v = mem_read(m, NULL, addr, size, &k);
    if (ok)
        *ok = k;
    return v;
}

void sys_write(chip_t *m, uint32_t addr, uint32_t v, int size, bool *ok)
{
    bool k = true;
    mem_write(m, NULL, addr, v, size, &k);
    if (ok)
        *ok = k;
}

void chip_init(chip_t *m, const board_info_t *b)
{
    memset(m, 0, sizeof(*m));
    g_chip = m;
    m->board = b;
    pthread_mutex_init(&m->power_lock, NULL);
    m->flash_size = b->flash_size;
    m->flash = malloc(m->flash_size + 0x10000);
    memset(m->flash, 0xff, m->flash_size + 0x10000);
    if (b->psram_size) {
        m->psram_size = b->psram_size;
        m->psram = calloc(1, m->psram_size + 0x10000);
    }
    m->rp2040 = b->rp2040;
    m->sram_size = b->rp2040 ? 0x42000u : SRAM_SIZE;
    m->xip_cached_top = b->rp2040 ? 0x12 : 0x14;
    m->rng = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < 2; i++) {
        m->core[i].m = m;
        m->core[i].id = i;
        m->core[i].v8 = !b->rp2040;
    }
    // The RP2040 runs its own ROM, which the caller loads.
    if (!b->rp2040) {
        rom_build(m);
        memcpy(m->rom + ROM_SIZE, m->rom, ROM_SIZE);
    }
    m->now = 0;
}

void chip_free(chip_t *m)
{
    // Models keep some of their events outside the chip, still marked as queued.
    while (m->events)
        ev_cancel(m, m->events);
    free(m->flash);
    free(m->psram);
    m->flash = m->psram = NULL;
    pthread_mutex_destroy(&m->power_lock);
    cpu_flush_code();
}

// The lines as the NVICs number them, from the ones the models raise.
static void sync_irq_ext(chip_t *m)
{
    m->irq_ext = 0;
    for (int core = 0; core < 2; core++)
        m->irq_core_ext[core] = 0;
    for (unsigned irq = 0; irq < NUM_IRQ; irq++) {
        if ((m->irq_lines >> irq) & 1)
            m->irq_ext |= ext_bit(m, irq, 0);
        for (int core = 0; core < 2; core++)
            if ((m->irq_core[core] >> irq) & 1)
                m->irq_core_ext[core] |= ext_bit(m, irq, core);
    }
}

static void fill_noise(uint8_t *p, size_t n, uint32_t seed)
{
    for (size_t i = 0; i + 4 <= n; i += 4) {
        seed = seed * 1664525u + 1013904223u;
        wr32le(p + i, seed);
    }
}

void chip_reset(chip_t *m, bool watchdog)
{
    m->code_gen++;
    while (m->events)
        ev_cancel(m, m->events);
    m->irq_lines = 0;
    memset(m->dreq, 0, sizeof(m->dreq));
    m->resets = 0x1fffffffu;
    m->psm_frce_on = m->psm_frce_off = m->psm_wdsel = 0;
    memset(m->clk, 0, sizeof(m->clk));
    for (int i = 0; i < 10; i++)
        m->clk[i].div = 1u << 16;
    m->clk_resus = 0xff;
    m->clk_wake[0] = m->clk_wake[1] = 0xffffffffu;
    m->clk_sleep[0] = m->clk_sleep[1] = 0xffffffffu;
    m->xosc_ctrl = 0;
    m->xosc_startup = 0xc4;
    memset(m->rosc_regs, 0, sizeof(m->rosc_regs));
    m->rosc_regs[0] = 0xfabaa0;
    m->rosc_regs[4] = 0xaa0;
    m->pll_sys = (pll_t){.cs = 1, .pwr = 0x2d, .fbdiv = 0, .prim = 0x77000};
    m->pll_usb = m->pll_sys;
    memset(m->syscfg, 0, sizeof(m->syscfg));
    memset(m->busctrl, 0, sizeof(m->busctrl));
    memset(m->ticks, 0, sizeof(m->ticks));
    memset(m->otp_regs, 0, sizeof(m->otp_regs));
    memset(m->trng_regs, 0, sizeof(m->trng_regs));
    m->bootlock_stat = 0xff;
    if (!watchdog) {
        memset(m->wd_scratch, 0, sizeof(m->wd_scratch));
        m->wd_reason = 0;
        memset(m->powman, 0, sizeof(m->powman));
        m->bootram_write_once[0] = m->bootram_write_once[1] = 0;
    }
    m->wd_ctrl = 0x07000000u;
    m->wd_load = 0;
    for (int i = 0; i < 2; i++)
        timer_reset(m, &m->timer[i], i);
    gpio_reset(m);
    // WL_REG_ON falls back to its pull-down with the rest of the pins, which powers the radio off.
    cyw43_reset(m);
    sio_reset(m);
    for (int i = 0; i < 2; i++) {
        pl022_reset(&m->spi[i], i);
        dw_i2c_reset(&m->i2c[i], i);
        pl011_reset(&m->uart[i], i);
    }
    for (int i = 0; i < 3; i++)
        pio_reset(&m->pio[i], i);
    dma_reset(&m->dma);
    pwm_reset(&m->pwm);
    qmi_reset(m);
    m->xip_ctrl = 0x83;
    xip_flush(m);
    m->adc_cs = m->adc_result = m->adc_fcs = m->adc_div = m->adc_inte = m->adc_intf = 0;
    memset(m->usb_regs, 0, sizeof(m->usb_regs));
    memset(m->dcp, 0, sizeof(m->dcp));
    dcp_reset();
    if (m->rp2040)
        rp2040_reset(m, !watchdog);
    chip_update_clocks(m);
    chip_rebuild_pages(m);
    sync_irq_ext(m);
    for (int i = 0; i < 2; i++) {
        cpu_t *c = &m->core[i];
        c->t = m->now;
        cpu_reset(c);
        if (!m->rp2040)
            rom_core_start(c);
    }
    m->boot_count++;
}

void chip_power_on(chip_t *m)
{
    m->code_gen++;
    fill_noise(m->sram, SRAM_SIZE, (uint32_t)(m->now >> 7) ^ 0x2350c0deu);
    if (m->psram)
        fill_noise(m->psram, m->psram_size, 0x5eed1234u);
    chip_reset(m, false);
}
