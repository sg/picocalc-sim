#include <string.h>

#include "chip.h"

// The QSPI memory interface: memory-mapped windows with ATRANS address translation onto the flash
// (CS0) and PSRAM (CS1), and direct mode, through which firmware sends raw commands. The flash
// answers like a Winbond W25Q, the PSRAM like an APS6404L.

enum { PH_CMD, PH_ADDR, PH_DUMMY, PH_DATA_IN, PH_DATA_OUT, PH_IGNORE };

typedef struct {
    int phase;
    uint8_t cmd;
    uint32_t addr;
    int addr_left, dummy_left;
    int out_pos;
} qspi_dev_t;

static qspi_dev_t devs[2];
static bool txn_open[2];

static void dev_begin(int cs)
{
    devs[cs] = (qspi_dev_t){.phase = PH_CMD};
    txn_open[cs] = true;
}

static void dev_end(chip_t *m, int cs)
{
    qspi_dev_t *d = &devs[cs];
    if (cs == 0 && d->phase != PH_CMD && m->qmi.wel) {
        switch (d->cmd) {
        case 0x20: flash_erase(m, d->addr & ~0xfffu, 0x1000); m->qmi.wel = false; break;
        case 0x52: flash_erase(m, d->addr & ~0x7fffu, 0x8000); m->qmi.wel = false; break;
        case 0xd8: flash_erase(m, d->addr & ~0xffffu, 0x10000); m->qmi.wel = false; break;
        case 0x60:
        case 0xc7: flash_erase(m, 0, m->flash_size); m->qmi.wel = false; break;
        case 0x02: m->qmi.wel = false; break;
        default: break;
        }
    }
    txn_open[cs] = false;
}

static uint8_t flash_byte(chip_t *m, uint8_t mosi)
{
    qspi_dev_t *d = &devs[0];
    qmi_t *q = &m->qmi;
    switch (d->phase) {
    case PH_CMD:
        d->cmd = mosi;
        switch (mosi) {
        case 0x06: q->wel = true; d->phase = PH_IGNORE; break;
        case 0x04: q->wel = false; d->phase = PH_IGNORE; break;
        case 0x05:
        case 0x35:
        case 0x15:
        case 0x9f: d->phase = PH_DATA_OUT; d->out_pos = 0; break;
        case 0x01:
        case 0x31:
        case 0x11: d->phase = PH_DATA_IN; break;
        case 0x03:
        case 0x0b:
        case 0x02:
        case 0x20:
        case 0x52:
        case 0xd8:
        case 0x5a:
        case 0x4b:
            d->phase = PH_ADDR;
            d->addr = 0;
            d->addr_left = 3;
            d->dummy_left = (mosi == 0x0b || mosi == 0x5a) ? 1 : (mosi == 0x4b ? 1 : 0);
            break;
        default: d->phase = PH_IGNORE; break;
        }
        return 0xff;
    case PH_ADDR:
        d->addr = (d->addr << 8) | mosi;
        if (--d->addr_left == 0) {
            if (d->cmd == 0x20 || d->cmd == 0x52 || d->cmd == 0xd8)
                d->phase = PH_IGNORE;
            else if (d->cmd == 0x02)
                d->phase = PH_DATA_IN;
            else
                d->phase = d->dummy_left ? PH_DUMMY : PH_DATA_OUT;
            d->out_pos = 0;
        }
        return 0xff;
    case PH_DUMMY:
        if (--d->dummy_left == 0)
            d->phase = PH_DATA_OUT;
        return 0xff;
    case PH_DATA_IN:
        if (d->cmd == 0x02) {
            if (q->wel) {
                uint32_t a = (d->addr & ~0xffu) | ((d->addr + (uint32_t)d->out_pos) & 0xff);
                flash_program(m, a, &mosi, 1);
            }
            d->out_pos++;
        } else if (d->cmd == 0x01) {
            q->sr1 = mosi & 0xfc;
            d->cmd = 0x31;
        } else if (d->cmd == 0x31) {
            q->sr2 = mosi;
        } else {
            q->sr3 = mosi;
        }
        return 0xff;
    case PH_DATA_OUT: {
        int i = d->out_pos++;
        switch (d->cmd) {
        case 0x05: return (uint8_t)(q->sr1 | (q->wel ? 2 : 0));
        case 0x35: return q->sr2;
        case 0x15: return q->sr3;
        case 0x9f: {
            uint8_t cap = 0x15;
            for (uint32_t s = m->flash_size; s > 1; s >>= 1)
                cap++;
            cap -= 0x15 - 0x15;
            static uint8_t id[3];
            id[0] = 0xef;
            id[1] = 0x40;
            uint8_t log2 = 0;
            for (uint32_t s = m->flash_size; s > 1; s >>= 1)
                log2++;
            id[2] = log2;
            (void)cap;
            return i < 3 ? id[i] : 0xff;
        }
        case 0x4b: {
            static const uint8_t uid[8] = {0xe6, 0x63, 0x58, 0x98, 0x23, 0x5a, 0x7c, 0x2e};
            return i < 8 ? uid[i] : 0xff;
        }
        case 0x03:
        case 0x0b: return m->flash[(d->addr + (uint32_t)i) & (m->flash_size - 1)];
        default: return 0xff;
        }
    }
    default:
        return 0xff;
    }
}

static uint8_t psram_byte(chip_t *m, uint8_t mosi)
{
    qspi_dev_t *d = &devs[1];
    if (!m->psram)
        return 0xff;
    switch (d->phase) {
    case PH_CMD:
        d->cmd = mosi;
        switch (mosi) {
        case 0x35: m->qmi.psram_qpi = true; d->phase = PH_IGNORE; break;
        case 0xf5: m->qmi.psram_qpi = false; d->phase = PH_IGNORE; break;
        case 0x9f:
        case 0x03:
        case 0x0b:
        case 0xeb:
        case 0x02:
        case 0x38:
            d->phase = PH_ADDR;
            d->addr = 0;
            d->addr_left = 3;
            d->dummy_left = mosi == 0x0b ? 1 : mosi == 0xeb ? 3 : 0;
            break;
        default: d->phase = PH_IGNORE; break;
        }
        return 0xff;
    case PH_ADDR:
        d->addr = (d->addr << 8) | mosi;
        if (--d->addr_left == 0) {
            d->phase = (d->cmd == 0x02 || d->cmd == 0x38) ? PH_DATA_IN : d->dummy_left ? PH_DUMMY : PH_DATA_OUT;
            d->out_pos = 0;
        }
        return 0xff;
    case PH_DUMMY:
        if (--d->dummy_left == 0)
            d->phase = PH_DATA_OUT;
        return 0xff;
    case PH_DATA_IN:
        m->psram[(d->addr + (uint32_t)d->out_pos++) % m->psram_size] = mosi;
        return 0xff;
    case PH_DATA_OUT: {
        int i = d->out_pos++;
        if (d->cmd == 0x9f) {
            static const uint8_t id[8] = {0x0d, 0x5d, 0x40, 0x12, 0x34, 0x56, 0x78, 0x9a};
            return id[i & 7];
        }
        return m->psram[(d->addr + (uint32_t)i) % m->psram_size];
    }
    default:
        return 0xff;
    }
}

void qmi_reset(chip_t *m)
{
    qmi_t *q = &m->qmi;
    memset(q, 0, sizeof(*q));
    for (int i = 0; i < 8; i++)
        q->atrans[i] = (0x400u << 16) | ((uint32_t)(i & 3) * 0x400u);
    q->m[0][0] = 0x40000004u;
    q->m[1][0] = 0x40000004u;
    q->sr2 = 0x02;
    dev_begin(0);
    dev_begin(1);
    txn_open[0] = txn_open[1] = false;
}

static bool cs_asserted(qmi_t *q, int cs)
{
    uint32_t csr = q->direct_csr;
    if (!(csr & 1))
        return false;
    return (csr & (1u << (2 + cs))) || (csr & (1u << (6 + cs)));
}

static void close_auto(chip_t *m)
{
    qmi_t *q = &m->qmi;
    for (int cs = 0; cs < 2; cs++)
        if (txn_open[cs] && !(q->direct_csr & (1u << (2 + cs))))
            dev_end(m, cs);
}

uint32_t qmi_read(chip_t *m, uint32_t off)
{
    qmi_t *q = &m->qmi;
    switch (off) {
    case 0x00: {
        // Firmware polls BUSY before deasserting chip select, so a poll marks the end of an
        // automatically-selected burst.
        close_auto(m);
        uint32_t v = q->direct_csr & ~((1u << 1) | (1u << 10) | (1u << 11) | (1u << 16) | (1u << 17));
        v |= 1u << 11;
        if (q->rxn == 0)
            v |= 1u << 17;
        if (q->rxn >= 4)
            v |= 1u << 16;
        return v;
    }
    case 0x08: {
        if (!q->rxn)
            return 0;
        uint32_t v = q->direct_rx[0];
        memmove(q->direct_rx, q->direct_rx + 1, sizeof(q->direct_rx) - sizeof(uint32_t));
        q->rxn--;
        return v;
    }
    default:
        if (off >= 0x0c && off < 0x34)
            return q->m[(off - 0x0c) / 20][((off - 0x0c) % 20) / 4];
        if (off >= 0x34 && off < 0x54)
            return q->atrans[(off - 0x34) >> 2];
        return 0;
    }
}

void qmi_write(chip_t *m, uint32_t off, uint32_t v)
{
    qmi_t *q = &m->qmi;
    switch (off) {
    case 0x00: {
        uint32_t old = q->direct_csr;
        q->direct_csr = v & ~((1u << 1) | (1u << 10) | (1u << 11) | (1u << 16) | (1u << 17));
        for (int cs = 0; cs < 2; cs++) {
            bool was = (old & 1) && (old & (1u << (2 + cs)));
            bool now = (v & 1) && (v & (1u << (2 + cs)));
            if (was && !now && txn_open[cs])
                dev_end(m, cs);
            if (!(v & 1) && txn_open[cs])
                dev_end(m, cs);
        }
        if (!(v & 1))
            q->rxn = 0;
        break;
    }
    case 0x04: {
        bool wide = v & (1u << 18), nopush = v & (1u << 20);
        uint32_t rx = 0;
        int nbytes = wide ? 2 : 1;
        for (int b = 0; b < nbytes; b++) {
            uint8_t mosi = (uint8_t)(wide ? (v >> (8 * (1 - b))) : v);
            uint8_t miso = 0xff;
            for (int cs = 0; cs < 2; cs++) {
                if (!cs_asserted(q, cs))
                    continue;
                if (!txn_open[cs])
                    dev_begin(cs);
                miso &= cs == 0 ? flash_byte(m, mosi) : psram_byte(m, mosi);
            }
            rx = (rx << 8) | miso;
        }
        if (!nopush && q->rxn < 4)
            q->direct_rx[q->rxn++] = rx;
        break;
    }
    default:
        if (off >= 0x0c && off < 0x34) {
            q->m[(off - 0x0c) / 20][((off - 0x0c) % 20) / 4] = v;
        } else if (off >= 0x34 && off < 0x54) {
            q->atrans[(off - 0x34) >> 2] = v & 0x07ff0fffu;
            chip_rebuild_pages(m);
        }
        break;
    }
}

uint32_t xipctrl_read(chip_t *m, uint32_t off)
{
    switch (off) {
    case 0x00: return m->xip_ctrl;
    case 0x08: return 0x6;
    case 0x0c: return m->xip_ctr_hit;
    case 0x10: return m->xip_ctr_acc;
    case 0x14: return m->xip_stream_addr;
    case 0x18: return m->xip_stream_ctr;
    default: return 0;
    }
}

void xipctrl_write(chip_t *m, uint32_t off, uint32_t v)
{
    switch (off) {
    case 0x00:
        m->xip_ctrl = v;
        break;
    case 0x0c: m->xip_ctr_hit = 0; break;
    case 0x10: m->xip_ctr_acc = 0; break;
    case 0x14: m->xip_stream_addr = v; break;
    case 0x18: m->xip_stream_ctr = v; break;
    default: break;
    }
}

static void mark_dirty(chip_t *m, uint32_t off, uint32_t len)
{
    if (!m->flash_dirty) {
        m->flash_dirty_lo = off;
        m->flash_dirty_hi = off + len;
    } else {
        if (off < m->flash_dirty_lo)
            m->flash_dirty_lo = off;
        if (off + len > m->flash_dirty_hi)
            m->flash_dirty_hi = off + len;
    }
    m->flash_dirty = true;
}

void flash_erase(chip_t *m, uint32_t off, uint32_t len)
{
    if (off >= m->flash_size)
        return;
    if (off + len > m->flash_size)
        len = m->flash_size - off;
    memset(m->flash + off, 0xff, len);
    mark_dirty(m, off, len);
}

void flash_program(chip_t *m, uint32_t off, const uint8_t *src, uint32_t len)
{
    if (off >= m->flash_size)
        return;
    if (off + len > m->flash_size)
        len = m->flash_size - off;
    for (uint32_t i = 0; i < len; i++)
        m->flash[off + i] &= src[i];
    mark_dirty(m, off, len);
}
