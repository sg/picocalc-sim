#include <string.h>

#include "chip.h"

// The RP2350's redundancy coprocessor (coprocessor 7): hardware-checked assertions that the boot
// ROM relies on, as the datasheet's section 3.6.3 describes them.

#define RCP_TRUE 0xa500a500u
#define RCP_FALSE 0x00c300c3u

void rcp_reset(chip_t *m)
{
    memset(&m->rcp, 0, sizeof(m->rcp));
}

void rcp_set_salts(chip_t *m, uint64_t salt0, uint64_t salt1)
{
    m->rcp.salt[0] = salt0;
    m->rcp.salt[1] = salt1;
    m->rcp.salt_valid[0] = m->rcp.salt_valid[1] = true;
}

static void stall(cpu_t *c, uint32_t pc, const char *why)
{
    emu_log("core %d stalled by the RCP: %s at pc=%08x", c->id, why, pc);
    c->r[15] = pc;
    c->state = CPU_STALL;
}

// A fault on either core faults both: each takes NMI, and its next RCP instruction stalls.
static void fault(cpu_t *c, uint32_t pc, const char *why)
{
    chip_t *m = c->m;
    emu_log("core %d RCP fault: %s at pc=%08x", c->id, why, pc);
    for (int i = 0; i < 2; i++) {
        m->rcp.fault[i] = true;
        m->core[i].pend_nmi = true;
        cpu_wake(&m->core[i], false);
    }
}

static uint32_t canary(uint64_t salt, uint8_t tag)
{
    uint32_t expanded = tag | (uint32_t)(uint8_t)~tag << 8 | (uint32_t)tag << 16;
    expanded &= 0xff0000u | ((uint32_t)(salt >> 24) & 0xffffu);
    return ((expanded ^ (uint32_t)salt) & 0xffffffu) << 8;
}

static unsigned majority3(uint32_t s, unsigned a, unsigned b, unsigned c)
{
    unsigned n = ((s >> a) & 1) + ((s >> b) & 1) + ((s >> c) & 1);
    return n >= 2;
}

static unsigned lfsr_step(unsigned s, unsigned width, unsigned taps)
{
    unsigned mask = (1u << width) - 1;
    unsigned in = s ? (unsigned)__builtin_parity(s & taps) : 1u;
    return ((s << 1) | in) & mask;
}

// The delay PRNG: four LFSRs in salt bits 63:40 and a non-linear output, per datasheet table 119.
static uint8_t random_byte(chip_t *m, int core)
{
    static const uint8_t xor_taps[8][6] = {
        {11, 3, 4, 19, 10, 14}, {4, 16, 11, 18, 9, 6},  {15, 13, 20, 21, 8, 12}, {23, 12, 7, 16, 14, 5},
        {4, 19, 17, 0, 18, 7},  {7, 5, 2, 18, 11, 1},   {14, 21, 19, 6, 16, 13}, {7, 17, 6, 16, 13, 8},
    };
    static const uint8_t maj_taps[8][3] = {
        {1, 2, 9}, {14, 21, 16}, {7, 22, 9}, {17, 3, 15}, {18, 11, 3}, {18, 14, 7}, {4, 14, 6}, {9, 12, 21},
    };
    uint32_t s = (uint32_t)(m->rcp.salt[core] >> 40) & 0xffffffu;
    uint8_t out = 0;
    for (int bit = 0; bit < 8; bit++) {
        unsigned v = majority3(s, maj_taps[bit][0], maj_taps[bit][1], maj_taps[bit][2]);
        for (int k = 0; k < 6; k++)
            v ^= (s >> xor_taps[bit][k]) & 1;
        out |= (uint8_t)(v << bit);
    }
    uint32_t next = lfsr_step(s >> 20, 4, 0xc) << 20 | lfsr_step((s >> 15) & 0x1f, 5, 0x14) << 15 |
                    lfsr_step((s >> 8) & 0x7f, 7, 0x60) << 8 | lfsr_step(s & 0xff, 8, 0xb4);
    m->rcp.salt[core] = (m->rcp.salt[core] & 0xffffffffffull) | (uint64_t)next << 40;
    return out;
}

static bool is_bool(uint32_t v)
{
    return v == RCP_TRUE || v == RCP_FALSE;
}

enum { BAD, MCR, MRC, MCRR, CDP };

static bool rcp_valid(int kind, unsigned opc1, unsigned crn, unsigned crm, unsigned rt)
{
    bool untagged = !crn && !crm;
    switch (kind) {
    case MRC: return opc1 == 0 || (opc1 <= 2 && untagged);
    case MCR: return opc1 == 0 || opc1 == 4 || opc1 == 5 || (opc1 <= 3 && untagged);
    case MCRR: return (opc1 == 8 && (crm & 7) <= 1) || (opc1 <= 7 && !(crm & 7));
    case CDP: return opc1 == 0 && untagged && !rt;
    default: return false;
    }
}

void rcp_exec(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    chip_t *m = c->m;
    int core = c->id;
    uint32_t *r = c->r;
    bool delay = !(hw1 & 0x1000);
    if (m->rcp.fault[core]) {
        stall(c, pc, "an RCP instruction after an RCP fault");
        return;
    }

    int kind = BAD;
    unsigned opc1 = 0, opc2 = 0, crn = 0, crm = hw2 & 0xf, rt = (hw2 >> 12) & 0xf, rt2 = hw1 & 0xf;
    if ((hw1 & 0x0fe0) == 0x0c40) {
        kind = (hw1 & 0x10) ? BAD : MCRR;
        opc1 = (hw2 >> 4) & 0xf;
    } else if ((hw1 & 0x0f00) == 0x0e00) {
        crn = hw1 & 0xf;
        opc2 = (hw2 >> 5) & 7;
        if (hw2 & 0x10) {
            kind = (hw1 & 0x10) ? MRC : MCR;
            opc1 = (hw1 >> 5) & 7;
        } else {
            kind = CDP;
            opc1 = (hw1 >> 4) & 0xf;
        }
    }
    // Every opcode the datasheet lists has an odd count of Opc1 ones plus the parity bit (Opc2
    // bit 0, or CRm bit 3 for mcrr), though its text says even.
    unsigned parity_bit = kind == MCRR ? (crm >> 3) & 1 : opc2 & 1;
    bool parity_ok = (__builtin_popcount(opc1) + parity_bit) & 1;
    // Non-secure code reads only zeroes, triggers no faults and sees no delays (datasheet section
    // 3.6.3.2), and an instruction the RCP rejects is undefined to it.
    if (c->ns) {
        if (!parity_ok || !rcp_valid(kind, opc1, crn, crm, rt))
            cpu_undefined(c, pc, (uint32_t)hw1 << 16 | hw2);
        else if (kind == MRC && rt == 15)
            c->apsr &= 0x0fffffffu;
        else if (kind == MRC)
            r[rt] = 0;
        return;
    }
    if (kind == BAD || !parity_ok) {
        fault(c, pc, "invalid instruction");
        return;
    }
    uint8_t tag = (uint8_t)(crn << 4 | crm);
    bool salted = m->rcp.salt_valid[core];
    bool random = kind == MRC && opc1 == 2 && !crn && !crm;
    if (delay && !random)
        c->cyc += random_byte(m, core) & 0x7f;

    if (kind == MRC && opc1 == 1 && !crn && !crm) {
        uint32_t v = salted ? RCP_TRUE : RCP_FALSE;
        if (rt == 15)
            c->apsr = (c->apsr & 0x0fffffffu) | (v & 0xf0000000u);
        else
            r[rt] = v;
        return;
    }
    if (kind == MCRR && opc1 == 8 && (crm & 7) <= 1) {
        int which = crm & 7;
        if (m->rcp.salt_valid[which]) {
            fault(c, pc, "salt written twice");
            return;
        }
        m->rcp.salt[which] = r[rt] | (uint64_t)r[rt2] << 32;
        m->rcp.salt_valid[which] = true;
        return;
    }
    if (!salted) {
        fault(c, pc, "used before its salt was set");
        return;
    }
    if (kind == CDP && opc1 == 0 && !crn && !crm && !((hw2 >> 12) & 0xf)) {
        stall(c, pc, "rcp_panic");
        for (int i = 0; i < 2; i++)
            if (i != core) {
                m->rcp.fault[i] = true;
                m->core[i].pend_nmi = true;
                cpu_wake(&m->core[i], false);
            }
        return;
    }
    if (random) {
        uint32_t v = random_byte(m, core);
        if (rt == 15)
            c->apsr = (c->apsr & 0x0fffffffu) | (v & 0xf0000000u);
        else
            r[rt] = v;
        return;
    }
    bool ok = true, known = true;
    uint32_t a = r[rt], b = r[rt2];
    if (kind == MRC && opc1 == 0) {
        uint32_t v = canary(m->rcp.salt[core], tag);
        if (rt == 15)
            c->apsr = (c->apsr & 0x0fffffffu) | (v & 0xf0000000u);
        else
            r[rt] = v;
    } else if (kind == MCR) {
        bool untagged = !crn && !crm;
        switch (opc1) {
        case 0: ok = a == canary(m->rcp.salt[core], tag); break;
        case 1: known = untagged; ok = is_bool(a); break;
        case 2: known = untagged; ok = a == RCP_TRUE; break;
        case 3: known = untagged; ok = a == RCP_FALSE; break;
        case 4: m->rcp.count[core] = tag; break;
        case 5: ok = m->rcp.count[core]++ == tag; break;
        default: known = false; break;
        }
    } else if (kind == MCRR && !(crm & 7)) {
        switch (opc1) {
        case 0: ok = is_bool(a) && is_bool(b); break;
        case 1: ok = a == RCP_TRUE && b == RCP_TRUE; break;
        case 2: ok = is_bool(a) && is_bool(b) && (a == RCP_TRUE || b == RCP_TRUE); break;
        case 3: ok = is_bool(a ^ b); break;
        case 4: ok = (a ^ b) == RCP_TRUE; break;
        case 5: ok = (a ^ b) == RCP_FALSE; break;
        case 6: ok = (a ^ b) == 0x96009600u; break;
        case 7: ok = a == b; break;
        default: known = false; break;
        }
    } else {
        known = false;
    }
    if (!known)
        fault(c, pc, "invalid instruction");
    else if (!ok)
        fault(c, pc, "assertion failed");
}
