// Thumb-2 32-bit instructions for ARMv8-M Mainline with the DSP extension. Included by cpu.c.

static void exec_fp(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2);

static int32_t sat_signed(cpu_t *c, int64_t v, unsigned bits)
{
    int64_t hi = ((int64_t)1 << (bits - 1)) - 1, lo = -((int64_t)1 << (bits - 1));
    if (v > hi) {
        c->apsr |= F_Q;
        return (int32_t)hi;
    }
    if (v < lo) {
        c->apsr |= F_Q;
        return (int32_t)lo;
    }
    return (int32_t)v;
}

static uint32_t sat_unsigned(cpu_t *c, int64_t v, unsigned bits)
{
    int64_t hi = bits >= 32 ? 0xffffffffll : (((int64_t)1 << bits) - 1);
    if (v > hi) {
        c->apsr |= F_Q;
        return (uint32_t)hi;
    }
    if (v < 0) {
        c->apsr |= F_Q;
        return 0;
    }
    return (uint32_t)v;
}

static void dp_op(cpu_t *c, unsigned op, unsigned rd, unsigned rn, bool s, uint32_t a, uint32_t b, uint32_t carry,
                  uint32_t pc)
{
    uint32_t *r = c->r;
    uint32_t res;
    uint32_t cin = (c->apsr & F_C) ? 1 : 0;
    switch (op) {
    case 0x0:
        res = a & b;
        if (s)
            set_nzc(c, res, carry);
        if (rd != 15)
            r[rd] = res;
        return;
    case 0x1:
        res = a & ~b;
        break;
    case 0x2:
        res = rn == 15 ? b : (a | b);
        break;
    case 0x3:
        res = rn == 15 ? ~b : (a | ~b);
        break;
    case 0x4:
        res = a ^ b;
        if (s)
            set_nzc(c, res, carry);
        if (rd != 15)
            r[rd] = res;
        return;
    case 0x8:
        res = add_c(c, a, b, 0, s);
        if (rd != 15)
            r[rd] = res;
        return;
    case 0xa:
        r[rd] = add_c(c, a, b, cin, s);
        return;
    case 0xb:
        r[rd] = add_c(c, a, ~b, cin, s);
        return;
    case 0xd:
        res = add_c(c, a, ~b, 1, s);
        if (rd != 15)
            r[rd] = res;
        return;
    case 0xe:
        r[rd] = add_c(c, b, ~a, 1, s);
        return;
    default:
        undefined(c, pc, op);
        return;
    }
    if (s)
        set_nzc(c, res, carry);
    r[rd] = res;
}

static void dp_modified_imm(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    unsigned op = (hw1 >> 5) & 0xf, rn = hw1 & 0xf, rd = (hw2 >> 8) & 0xf;
    bool s = hw1 & 0x10;
    uint32_t imm12 = ((uint32_t)(hw1 & 0x400) << 1) | ((hw2 >> 4) & 0x700) | (hw2 & 0xff);
    uint32_t carry;
    uint32_t imm = thumb_expand_imm_c(imm12, (c->apsr & F_C) ? 1 : 0, &carry);
    dp_op(c, op, rd, rn, s, c->r[rn], imm, carry, pc);
}

static void dp_plain_imm(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    uint32_t *r = c->r;
    unsigned op = (hw1 >> 4) & 0x1f, rn = hw1 & 0xf, rd = (hw2 >> 8) & 0xf;
    uint32_t imm12 = ((uint32_t)(hw1 & 0x400) << 1) | ((hw2 >> 4) & 0x700) | (hw2 & 0xff);
    unsigned imm5 = ((hw2 >> 10) & 0x1c) | ((hw2 >> 6) & 3);
    unsigned widthm1 = hw2 & 0x1f;
    switch (op) {
    case 0x00:
        r[rd] = (rn == 15 ? ((pc + 4) & ~3u) : r[rn]) + imm12;
        break;
    case 0x04:
        r[rd] = ((uint32_t)(hw1 & 0xf) << 12) | imm12;
        break;
    case 0x0a:
        r[rd] = (rn == 15 ? ((pc + 4) & ~3u) : r[rn]) - imm12;
        break;
    case 0x0c:
        r[rd] = (r[rd] & 0xffff) | (((uint32_t)(hw1 & 0xf) << 28) | (imm12 << 16));
        break;
    case 0x10:
    case 0x12: {
        bool asr = op & 2;
        unsigned sat = widthm1 + 1;
        if (asr && imm5 == 0) {
            int32_t lo = sat_signed(c, (int16_t)r[rn], sat);
            int32_t hi = sat_signed(c, (int16_t)(r[rn] >> 16), sat);
            r[rd] = ((uint32_t)lo & 0xffff) | ((uint32_t)hi << 16);
        } else {
            int64_t v = asr ? ((int64_t)(int32_t)r[rn] >> (imm5 ? imm5 : 32)) : (int64_t)(int32_t)(r[rn] << imm5);
            r[rd] = (uint32_t)sat_signed(c, v, sat);
        }
        break;
    }
    case 0x14: {
        unsigned lsb = imm5;
        uint32_t v = r[rn] >> lsb;
        unsigned w = widthm1 + 1;
        if (w < 32)
            v = (uint32_t)(((int32_t)(v << (32 - w))) >> (32 - w));
        r[rd] = v;
        break;
    }
    case 0x16: {
        unsigned lsb = imm5, msb = widthm1;
        if (msb >= lsb) {
            uint32_t mask = (uint32_t)((((uint64_t)1 << (msb - lsb + 1)) - 1) << lsb);
            uint32_t src = rn == 15 ? 0 : (r[rn] << lsb);
            r[rd] = (r[rd] & ~mask) | (src & mask);
        }
        break;
    }
    case 0x18:
    case 0x1a: {
        bool asr = op & 2;
        unsigned sat = widthm1;
        if (asr && imm5 == 0) {
            uint32_t lo = sat_unsigned(c, (int16_t)r[rn], sat);
            uint32_t hi = sat_unsigned(c, (int16_t)(r[rn] >> 16), sat);
            r[rd] = (lo & 0xffff) | (hi << 16);
        } else {
            int64_t v = asr ? ((int64_t)(int32_t)r[rn] >> (imm5 ? imm5 : 32)) : (int64_t)(int32_t)(r[rn] << imm5);
            r[rd] = sat_unsigned(c, v, sat);
        }
        break;
    }
    case 0x1c: {
        unsigned lsb = imm5, w = widthm1 + 1;
        uint32_t v = r[rn] >> lsb;
        if (w < 32)
            v &= (1u << w) - 1;
        r[rd] = v;
        break;
    }
    default:
        undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
        break;
    }
}

static void branch_misc(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    uint32_t *r = c->r;
    unsigned op1 = (hw2 >> 12) & 7;
    uint32_t s = (hw1 >> 10) & 1;
    if (op1 & 1) {
        uint32_t j1 = (hw2 >> 13) & 1, j2 = (hw2 >> 11) & 1;
        uint32_t i1 = !(j1 ^ s), i2 = !(j2 ^ s);
        uint32_t imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((uint32_t)(hw1 & 0x3ff) << 12) | ((uint32_t)(hw2 & 0x7ff) << 1);
        if (s)
            imm |= 0xfe000000u;
        if (op1 & 4) {
            r[14] = (pc + 4) | 1;
            c->cyc += 2;
        } else {
            c->cyc++;
        }
        r[15] = pc + 4 + imm;
        return;
    }
    if (op1 & 4) {
        undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
        return;
    }
    if (((hw1 >> 7) & 7) != 7) {
        unsigned cond = (hw1 >> 6) & 0xf;
        if (cond_pass(c, cond)) {
            uint32_t j1 = (hw2 >> 13) & 1, j2 = (hw2 >> 11) & 1;
            uint32_t imm = (s << 20) | (j2 << 19) | (j1 << 18) | ((uint32_t)(hw1 & 0x3f) << 12) | ((uint32_t)(hw2 & 0x7ff) << 1);
            if (s)
                imm |= 0xfff00000u;
            r[15] = pc + 4 + imm;
            c->cyc++;
        }
        return;
    }
    unsigned op = (hw1 >> 4) & 0x7f;
    if (op == 0x38 || op == 0x39) {
        write_special(c, hw2 & 0xff, r[hw1 & 0xf], (hw2 >> 10) & 3);
        return;
    }
    if (op == 0x3a) {
        switch (hw2 & 0xff) {
        case 0x02:
            if (c->event)
                c->event = false;
            else
                c->state = CPU_WFE;
            break;
        case 0x03:
            c->state = CPU_WFI;
            break;
        case 0x04:
            chip_sev(c->m, c->id);
            break;
        default:
            break;
        }
        return;
    }
    if (op == 0x3b) {
        if (((hw2 >> 4) & 0xf) == 2)
            c->excl_valid = false;
        c->cyc += 1;
        return;
    }
    if (op == 0x3e || op == 0x3f) {
        r[(hw2 >> 8) & 0xf] = read_special(c, hw2 & 0xff);
        return;
    }
    undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
}

static void ldm_stm(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    uint32_t *r = c->r;
    unsigned rn = hw1 & 0xf;
    bool load = hw1 & 0x10, wb = hw1 & 0x20, db = (hw1 & 0x180) == 0x100;
    uint32_t list = hw2;
    if (!load)
        list &= 0x5fff;
    unsigned n = (unsigned)__builtin_popcount(list);
    uint32_t base = r[rn];
    uint32_t a = db ? base - 4 * n : base;
    uint32_t end = db ? base - 4 * n : base + 4 * n;
    if (load) {
        uint32_t vals[16];
        for (int i = 0; i < 16; i++)
            if (list & (1u << i)) {
                vals[i] = ld32(c, a);
                a += 4;
            }
        if (c->fault)
            return;
        for (int i = 0; i < 15; i++)
            if (list & (1u << i))
                r[i] = vals[i];
        if (wb && !(list & (1u << rn)))
            r[rn] = end;
        if (list & 0x8000) {
            c->cyc += 2;
            branch_interworking(c, vals[15], pc);
        }
    } else {
        for (int i = 0; i < 15; i++)
            if (list & (1u << i)) {
                st32(c, a, r[i]);
                a += 4;
            }
        if (wb && !c->fault)
            r[rn] = end;
    }
}

static void excl_clear_others(cpu_t *c, uint32_t addr)
{
    for (int i = 0; i < 2; i++) {
        cpu_t *o = &c->m->core[i];
        if (o != c && o->excl_valid && ((o->excl_addr ^ addr) & ~7u) == 0)
            o->excl_valid = false;
    }
}

static void ldst_dual_excl(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    uint32_t *r = c->r;
    unsigned rn = hw1 & 0xf, rt = (hw2 >> 12) & 0xf, rt2 = (hw2 >> 8) & 0xf;
    unsigned op = (hw1 >> 4) & 0x1f;
    if ((hw1 & 0xfff0) == 0xe840 && (hw2 & 0xf03f) == 0xf000) {
        // TT: everything here is Secure, and an IDAU-exempt or secure address reports S=1.
        unsigned rd = (hw2 >> 8) & 0xf;
        r[rd] = (1u << 22) | (1u << 20) | (1u << 18);
        return;
    }
    if (hw1 == 0xe97f && hw2 == 0xe97f)
        return;
    if ((hw1 & 0xfff0) == 0xe840) {
        uint32_t a = r[rn] + ((hw2 & 0xff) << 2);
        unsigned rd = (hw2 >> 8) & 0xf;
        if (c->excl_valid && c->excl_addr == a) {
            st32(c, a, r[rt]);
            excl_clear_others(c, a);
            r[rd] = 0;
        } else {
            r[rd] = 1;
        }
        c->excl_valid = false;
        return;
    }
    if ((hw1 & 0xfff0) == 0xe850) {
        uint32_t a = r[rn] + ((hw2 & 0xff) << 2);
        r[rt] = ld32(c, a);
        c->excl_valid = true;
        c->excl_addr = a;
        return;
    }
    if ((hw1 & 0xfff0) == 0xe8c0 || (hw1 & 0xfff0) == 0xe8d0) {
        bool load = hw1 & 0x10;
        unsigned op3 = (hw2 >> 4) & 0xf;
        uint32_t a = r[rn];
        if (load && op3 <= 1) {
            if (rn == 15)
                a = pc + 4;
            uint32_t off = op3 ? (ld16(c, a + 2 * r[hw2 & 0xf]) << 1) : (ld8(c, a + r[hw2 & 0xf]) << 1);
            r[15] = pc + 4 + off;
            c->cyc += 2;
            return;
        }
        int size = (op3 & 3) == 0 ? 1 : (op3 & 3) == 1 ? 2 : 4;
        bool excl = (op3 & 4) != 0;
        if (rn == 15 && load) {
            undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
            return;
        }
        if (load) {
            uint32_t v = size == 1 ? ld8(c, a) : size == 2 ? ld16(c, a) : ld32(c, a);
            r[rt] = v;
            if (excl) {
                c->excl_valid = true;
                c->excl_addr = a;
            }
        } else {
            unsigned rd = hw2 & 0xf;
            if (excl) {
                if (c->excl_valid && c->excl_addr == a) {
                    if (size == 1)
                        st8(c, a, r[rt]);
                    else if (size == 2)
                        st16(c, a, r[rt]);
                    else
                        st32(c, a, r[rt]);
                    excl_clear_others(c, a);
                    r[rd] = 0;
                } else {
                    r[rd] = 1;
                }
                c->excl_valid = false;
            } else {
                if (size == 1)
                    st8(c, a, r[rt]);
                else if (size == 2)
                    st16(c, a, r[rt]);
                else
                    st32(c, a, r[rt]);
                excl_clear_others(c, a);
            }
        }
        return;
    }
    bool p = op & 0x10, u = op & 0x08, w = op & 0x02, l = op & 0x01;
    uint32_t imm = (hw2 & 0xff) << 2;
    uint32_t base = rn == 15 ? ((pc + 4) & ~3u) : r[rn];
    uint32_t offaddr = u ? base + imm : base - imm;
    uint32_t a = p ? offaddr : base;
    if (l) {
        uint32_t v1 = ld32(c, a), v2 = ld32(c, a + 4);
        if (c->fault)
            return;
        r[rt] = v1;
        r[rt2] = v2;
    } else {
        st32(c, a, r[rt]);
        st32(c, a + 4, r[rt2]);
    }
    if (w && !c->fault)
        r[rn] = offaddr;
}

static void dp_shifted_reg(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    uint32_t *r = c->r;
    unsigned op = (hw1 >> 5) & 0xf, rn = hw1 & 0xf, rd = (hw2 >> 8) & 0xf, rm = hw2 & 0xf;
    bool s = hw1 & 0x10;
    unsigned type = (hw2 >> 4) & 3, imm5 = ((hw2 >> 10) & 0x1c) | ((hw2 >> 6) & 3);
    uint32_t carry;
    uint32_t b = imm_shift_c(r[rm], type, imm5, (c->apsr & F_C) ? 1 : 0, &carry);
    if (op == 0x6) {
        uint32_t a = r[rn];
        if (type == 2)
            r[rd] = (a & 0xffff0000u) | (b & 0xffff);
        else
            r[rd] = (b & 0xffff0000u) | (a & 0xffff);
        return;
    }
    dp_op(c, op, rd, rn, s, rn == 15 ? 0 : r[rn], b, carry, pc);
}

static uint32_t parallel_addsub(cpu_t *c, unsigned op1, unsigned op2, uint32_t a, uint32_t b)
{
    bool uns = op2 & 4;
    unsigned kind = op2 & 3;
    uint32_t res = 0, ge = 0;
    if (op1 == 0 || op1 == 4) {
        for (int i = 0; i < 4; i++) {
            int32_t x = uns ? (int32_t)((a >> (8 * i)) & 0xff) : (int8_t)(a >> (8 * i));
            int32_t y = uns ? (int32_t)((b >> (8 * i)) & 0xff) : (int8_t)(b >> (8 * i));
            int32_t v = op1 == 0 ? x + y : x - y;
            int32_t o;
            if (kind == 0) {
                o = v;
                if (uns ? (op1 == 0 ? v >= 0x100 : v >= 0) : v >= 0)
                    ge |= 1u << i;
            } else if (kind == 1) {
                o = uns ? (v < 0 ? 0 : v > 255 ? 255 : v) : (v < -128 ? -128 : v > 127 ? 127 : v);
            } else {
                o = v >> 1;
            }
            res |= ((uint32_t)o & 0xff) << (8 * i);
        }
    } else {
        int32_t xl = uns ? (int32_t)(a & 0xffff) : (int16_t)a, xh = uns ? (int32_t)(a >> 16) : (int16_t)(a >> 16);
        int32_t yl = uns ? (int32_t)(b & 0xffff) : (int16_t)b, yh = uns ? (int32_t)(b >> 16) : (int16_t)(b >> 16);
        int32_t lo, hi;
        bool add_lo, add_hi;
        switch (op1) {
        case 1: lo = xl + yl; hi = xh + yh; add_lo = add_hi = true; break;
        case 2: lo = xl - yh; hi = xh + yl; add_lo = false; add_hi = true; break;
        case 6: lo = xl + yh; hi = xh - yl; add_lo = true; add_hi = false; break;
        default: lo = xl - yl; hi = xh - yh; add_lo = add_hi = false; break;
        }
        int32_t o[2] = {lo, hi};
        bool ad[2] = {add_lo, add_hi};
        for (int i = 0; i < 2; i++) {
            int32_t v = o[i];
            if (kind == 0) {
                if (uns ? (ad[i] ? v >= 0x10000 : v >= 0) : v >= 0)
                    ge |= 3u << (2 * i);
            } else if (kind == 1) {
                v = uns ? (v < 0 ? 0 : v > 65535 ? 65535 : v) : (v < -32768 ? -32768 : v > 32767 ? 32767 : v);
            } else {
                v >>= 1;
            }
            res |= ((uint32_t)v & 0xffff) << (16 * i);
        }
    }
    if (kind == 0)
        c->apsr = (c->apsr & ~0x000f0000u) | (ge << 16);
    return res;
}

static void dp_register(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    uint32_t *r = c->r;
    unsigned op1 = (hw1 >> 4) & 0xf, op2 = (hw2 >> 4) & 0xf;
    unsigned rn = hw1 & 0xf, rd = (hw2 >> 8) & 0xf, rm = hw2 & 0xf;
    if ((hw2 & 0xf000) != 0xf000) {
        undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
        return;
    }
    if (op2 == 0 && op1 < 8) {
        uint32_t co;
        uint32_t res = shift_c(r[rn], op1 >> 1, r[rm] & 0xff, (c->apsr & F_C) ? 1 : 0, &co);
        r[rd] = res;
        if (op1 & 1)
            set_nzc(c, res, co);
        return;
    }
    if (op1 < 8 && (op2 & 8)) {
        unsigned rot = (hw2 >> 4) & 3;
        uint32_t v = r[rm];
        if (rot)
            v = (v >> (8 * rot)) | (v << (32 - 8 * rot));
        uint32_t add = rn == 15 ? 0 : r[rn];
        switch (op1) {
        case 0: r[rd] = add + (uint32_t)(int32_t)(int16_t)v; break;
        case 1: r[rd] = add + (v & 0xffff); break;
        case 2: {
            uint32_t lo = (uint32_t)(int32_t)(int8_t)v, hi = (uint32_t)(int32_t)(int8_t)(v >> 16);
            r[rd] = (((add & 0xffff) + lo) & 0xffff) | ((((add >> 16) + hi) & 0xffff) << 16);
            break;
        }
        case 3: {
            uint32_t lo = v & 0xff, hi = (v >> 16) & 0xff;
            r[rd] = (((add & 0xffff) + lo) & 0xffff) | ((((add >> 16) + hi) & 0xffff) << 16);
            break;
        }
        case 4: r[rd] = add + (uint32_t)(int32_t)(int8_t)v; break;
        case 5: r[rd] = add + (v & 0xff); break;
        default: undefined(c, pc, ((uint32_t)hw1 << 16) | hw2); break;
        }
        return;
    }
    if (op1 >= 8 && op2 < 8) {
        r[rd] = parallel_addsub(c, op1 & 7, op2, r[rn], r[rm]);
        return;
    }
    if ((op1 & 0xc) == 8 && (op2 & 0xc) == 8) {
        unsigned o1 = op1 & 3, o2 = op2 & 3;
        uint32_t v = r[rm];
        switch (o1) {
        case 0: {
            int64_t a = (int32_t)r[rm], b = (int32_t)r[rn];
            if (o2 & 1)
                b = sat_signed(c, 2 * b, 32);
            r[rd] = (uint32_t)sat_signed(c, (o2 & 2) ? a - b : a + b, 32);
            break;
        }
        case 1:
            switch (o2) {
            case 0: r[rd] = __builtin_bswap32(v); break;
            case 1: r[rd] = ((v & 0xff00ff00u) >> 8) | ((v & 0x00ff00ffu) << 8); break;
            case 2: {
                uint32_t x = v;
                x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
                x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
                x = ((x >> 4) & 0x0f0f0f0fu) | ((x & 0x0f0f0f0fu) << 4);
                r[rd] = __builtin_bswap32(x);
                break;
            }
            default: r[rd] = (uint32_t)(int32_t)(int16_t)(((v & 0xff) << 8) | ((v >> 8) & 0xff)); break;
            }
            break;
        case 2: {
            uint32_t ge = (c->apsr >> 16) & 0xf, a = r[rn], res = 0;
            for (int i = 0; i < 4; i++)
                res |= ((ge >> i) & 1 ? a : v) & (0xffu << (8 * i));
            r[rd] = res;
            break;
        }
        default:
            r[rd] = v ? (uint32_t)__builtin_clz(v) : 32;
            break;
        }
        return;
    }
    undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
}

static inline int32_t half(uint32_t v, bool top)
{
    return top ? (int16_t)(v >> 16) : (int16_t)v;
}

static void multiply(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    uint32_t *r = c->r;
    unsigned op1 = (hw1 >> 4) & 7, op2 = (hw2 >> 4) & 3;
    unsigned rn = hw1 & 0xf, ra = (hw2 >> 12) & 0xf, rd = (hw2 >> 8) & 0xf, rm = hw2 & 0xf;
    uint32_t a = r[rn], b = r[rm], acc = ra == 15 ? 0 : r[ra];
    switch (op1) {
    case 0:
        if (op2 == 0)
            r[rd] = a * b + acc;
        else if (op2 == 1)
            r[rd] = r[ra] - a * b;
        else
            undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
        break;
    case 1: {
        int64_t p = (int64_t)half(a, op2 & 2) * half(b, op2 & 1);
        int64_t s = p + (int32_t)acc;
        if (ra != 15 && s != (int32_t)s)
            c->apsr |= F_Q;
        r[rd] = (uint32_t)s;
        break;
    }
    case 2:
    case 4: {
        bool x = op2 & 1;
        uint32_t bb = x ? ((b >> 16) | (b << 16)) : b;
        int64_t p1 = (int64_t)half(a, false) * half(bb, false);
        int64_t p2 = (int64_t)half(a, true) * half(bb, true);
        int64_t s = (op1 == 2 ? p1 + p2 : p1 - p2) + (int32_t)acc;
        if (s != (int32_t)s)
            c->apsr |= F_Q;
        r[rd] = (uint32_t)s;
        break;
    }
    case 3: {
        int64_t p = ((int64_t)(int32_t)a * half(b, op2 & 1)) >> 16;
        int64_t s = p + (int32_t)acc;
        if (ra != 15 && s != (int32_t)s)
            c->apsr |= F_Q;
        r[rd] = (uint32_t)s;
        break;
    }
    case 5:
    case 6: {
        int64_t p = (int64_t)(int32_t)a * (int32_t)b;
        int64_t v;
        if (op1 == 5)
            v = ((int64_t)acc << 32) + p;
        else
            v = ((int64_t)acc << 32) - p;
        if (op2 & 1)
            v += 0x80000000ll;
        r[rd] = (uint32_t)((uint64_t)v >> 32);
        break;
    }
    case 7: {
        uint32_t s = 0;
        for (int i = 0; i < 4; i++) {
            int d = (int)((a >> (8 * i)) & 0xff) - (int)((b >> (8 * i)) & 0xff);
            s += (uint32_t)(d < 0 ? -d : d);
        }
        r[rd] = s + acc;
        break;
    }
    }
}

static void long_multiply(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    uint32_t *r = c->r;
    unsigned op1 = (hw1 >> 4) & 7, op2 = (hw2 >> 4) & 0xf;
    unsigned rn = hw1 & 0xf, rlo = (hw2 >> 12) & 0xf, rhi = (hw2 >> 8) & 0xf, rm = hw2 & 0xf;
    uint32_t a = r[rn], b = r[rm];
    uint64_t acc = ((uint64_t)r[rhi] << 32) | r[rlo];
    if (op1 == 1 || op1 == 3) {
        unsigned rd = rhi;
        if (b == 0) {
            if (c->ccr & 0x10) {
                c->cfsr |= UFSR_DIVBYZERO;
                take_fault(c, EXC_USAGEFAULT, pc, "divide by zero");
                return;
            }
            r[rd] = 0;
        } else if (op1 == 1) {
            if ((int32_t)a == INT32_MIN && (int32_t)b == -1)
                r[rd] = (uint32_t)INT32_MIN;
            else
                r[rd] = (uint32_t)((int32_t)a / (int32_t)b);
        } else {
            r[rd] = a / b;
        }
        c->cyc += 4;
        return;
    }
    uint64_t res;
    switch (op1) {
    case 0: res = (uint64_t)((int64_t)(int32_t)a * (int32_t)b); break;
    case 2: res = (uint64_t)a * b; break;
    case 4:
        if (op2 == 0)
            res = acc + (uint64_t)((int64_t)(int32_t)a * (int32_t)b);
        else if ((op2 & 0xc) == 8)
            res = acc + (uint64_t)((int64_t)half(a, op2 & 2) * half(b, op2 & 1));
        else {
            uint32_t bb = (op2 & 1) ? ((b >> 16) | (b << 16)) : b;
            res = acc + (uint64_t)((int64_t)half(a, false) * half(bb, false) + (int64_t)half(a, true) * half(bb, true));
        }
        break;
    case 5: {
        uint32_t bb = (op2 & 1) ? ((b >> 16) | (b << 16)) : b;
        res = acc + (uint64_t)((int64_t)half(a, false) * half(bb, false) - (int64_t)half(a, true) * half(bb, true));
        break;
    }
    case 6:
        if (op2 == 6)
            res = (uint64_t)a * b + r[rlo] + r[rhi];
        else
            res = acc + (uint64_t)a * b;
        break;
    default:
        undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
        return;
    }
    r[rlo] = (uint32_t)res;
    r[rhi] = (uint32_t)(res >> 32);
    c->cyc++;
}

static void ldst_single(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    uint32_t *r = c->r;
    bool sign = hw1 & 0x100, load = hw1 & 0x10;
    unsigned size = (hw1 >> 5) & 3, rn = hw1 & 0xf, rt = (hw2 >> 12) & 0xf;
    uint32_t a;
    uint32_t wb_addr = 0;
    bool wb = false;
    if (size == 3) {
        undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
        return;
    }
    if (rn == 15) {
        if (!load) {
            undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
            return;
        }
        uint32_t base = (pc + 4) & ~3u;
        a = (hw1 & 0x80) ? base + (hw2 & 0xfff) : base - (hw2 & 0xfff);
    } else if (hw1 & 0x80) {
        a = r[rn] + (hw2 & 0xfff);
    } else if (hw2 & 0x800) {
        bool p = hw2 & 0x400, u = hw2 & 0x200, w = hw2 & 0x100;
        uint32_t imm = hw2 & 0xff;
        uint32_t off = u ? r[rn] + imm : r[rn] - imm;
        a = p ? off : r[rn];
        if (w) {
            wb = true;
            wb_addr = off;
        }
    } else if ((hw2 & 0xfc0) == 0) {
        a = r[rn] + (r[hw2 & 0xf] << ((hw2 >> 4) & 3));
    } else {
        undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
        return;
    }
    if (load) {
        if (rt == 15 && size != 2) {
            return;
        }
        uint32_t v;
        if (size == 0)
            v = sign ? (uint32_t)(int32_t)(int8_t)ld8(c, a) : ld8(c, a);
        else if (size == 1)
            v = sign ? (uint32_t)(int32_t)(int16_t)ld16u(c, a) : ld16u(c, a);
        else
            v = ld32u(c, a);
        if (c->fault)
            return;
        if (wb)
            r[rn] = wb_addr;
        if (rt == 15) {
            c->cyc += 2;
            branch_interworking(c, v, pc);
        } else {
            r[rt] = v;
        }
    } else {
        if (sign) {
            undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
            return;
        }
        if (size == 0)
            st8(c, a, r[rt]);
        else if (size == 1)
            st16u(c, a, r[rt]);
        else
            st32u(c, a, r[rt]);
        if (wb && !c->fault)
            r[rn] = wb_addr;
    }
}

static void coprocessor(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    uint32_t *r = c->r;
    unsigned cp = (hw2 >> 8) & 0xf;
    bool two = hw1 & 0x1000;
    if (!two && (cp == 10 || cp == 11)) {
        exec_fp(c, pc, hw1, hw2);
        return;
    }
    if (two && (hw1 & 0xff00) == 0xfe00 && (cp == 10 || cp == 11)) {
        exec_fp(c, pc, hw1, hw2);
        return;
    }
    if (cp == 7 && !c->m->rp2040) {
        rcp_exec(c, pc, hw1, hw2);
        return;
    }
    bool ok = false;
    if ((hw1 & 0x0fe0) == 0x0c40) {
        unsigned rt = (hw2 >> 12) & 0xf, rt2 = hw1 & 0xf, opc1 = (hw2 >> 4) & 0xf, crm = hw2 & 0xf;
        if (hw1 & 0x10) {
            uint32_t lo = 0, hi = 0;
            ok = cp == 4 ? dcp_mrrc(c, opc1, crm, &lo, &hi, two) : coproc_mrrc(c, cp, opc1, crm, &lo, &hi);
            if (ok) {
                r[rt] = lo;
                r[rt2] = hi;
            }
        } else {
            ok = cp == 4 ? dcp_mcrr(c, opc1, crm, r[rt], r[rt2]) : coproc_mcrr(c, cp, opc1, crm, r[rt], r[rt2]);
        }
    } else if ((hw1 & 0x0f00) == 0x0e00) {
        unsigned crn = hw1 & 0xf, crm = hw2 & 0xf, opc2 = (hw2 >> 5) & 7;
        if (hw2 & 0x10) {
            unsigned rt = (hw2 >> 12) & 0xf, opc1 = (hw1 >> 5) & 7;
            if (hw1 & 0x10) {
                uint32_t v = 0;
                ok = cp == 4 ? dcp_mrc(c, crm, opc2, &v, two) : coproc_mrc(c, cp, opc1, crn, crm, opc2, &v);
                if (ok) {
                    if (rt == 15)
                        c->apsr = (c->apsr & 0x0fffffffu) | (v & 0xf0000000u);
                    else
                        r[rt] = v;
                }
            } else {
                ok = coproc_mcr(c, cp, opc1, crn, crm, opc2, r[rt]);
            }
        } else {
            unsigned opc1 = (hw1 >> 4) & 0xf, crd = (hw2 >> 12) & 0xf;
            ok = cp == 4 ? dcp_cdp(c, opc1, crd, crn, crm, opc2) : coproc_cdp(c, cp, opc1, crd, crn, crm, opc2);
        }
    }
    if (!ok) {
        c->cfsr |= UFSR_NOCP;
        if (c->m->trace)
            emu_log("core %d coprocessor %u instruction %04x %04x at %08x not handled", c->id, cp, hw1, hw2, pc);
        take_fault(c, EXC_USAGEFAULT, pc, "coprocessor instruction");
    }
}

static void exec32(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    switch ((hw1 >> 11) & 3) {
    case 1:
        if ((hw1 & 0x0600) == 0x0000) {
            if (hw1 & 0x40)
                ldst_dual_excl(c, pc, hw1, hw2);
            else
                ldm_stm(c, pc, hw1, hw2);
        } else if ((hw1 & 0x0600) == 0x0200) {
            dp_shifted_reg(c, pc, hw1, hw2);
        } else {
            coprocessor(c, pc, hw1, hw2);
        }
        return;
    case 2:
        if (hw2 & 0x8000)
            branch_misc(c, pc, hw1, hw2);
        else if (hw1 & 0x0200)
            dp_plain_imm(c, pc, hw1, hw2);
        else
            dp_modified_imm(c, pc, hw1, hw2);
        return;
    case 3:
        if ((hw1 & 0x0e00) == 0x0800) {
            ldst_single(c, pc, hw1, hw2);
        } else if ((hw1 & 0x0f00) == 0x0a00) {
            dp_register(c, pc, hw1, hw2);
        } else if ((hw1 & 0x0f80) == 0x0b00) {
            multiply(c, pc, hw1, hw2);
        } else if ((hw1 & 0x0f80) == 0x0b80) {
            long_multiply(c, pc, hw1, hw2);
        } else if ((hw1 & 0x0c00) == 0x0c00) {
            coprocessor(c, pc, hw1, hw2);
        } else {
            undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
        }
        return;
    default:
        undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
        return;
    }
}
