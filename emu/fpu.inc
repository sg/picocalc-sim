// FPv5 single-precision floating point for the Cortex-M33. Included by cpu.c.

#include <fenv.h>

#define FPSCR_DN (1u << 25)
#define FPSCR_FZ (1u << 24)

static inline float s_get(cpu_t *c, unsigned i)
{
    float f;
    memcpy(&f, &c->s[i & 31], 4);
    return f;
}

static inline void s_set(cpu_t *c, unsigned i, float f)
{
    memcpy(&c->s[i & 31], &f, 4);
}

static inline uint32_t f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static inline float u2f(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static float fp_in(cpu_t *c, float f)
{
    if ((c->fpscr & FPSCR_FZ) && fpclassify(f) == FP_SUBNORMAL) {
        c->fpscr |= 1u << 7;
        return copysignf(0.0f, f);
    }
    return f;
}

static float fp_out(cpu_t *c, float f)
{
    if (isnan(f)) {
        if (c->fpscr & FPSCR_DN)
            return u2f(0x7fc00000u);
        return u2f(f2u(f) | 0x00400000u);
    }
    if ((c->fpscr & FPSCR_FZ) && fpclassify(f) == FP_SUBNORMAL) {
        c->fpscr |= 1u << 3;
        return copysignf(0.0f, f);
    }
    return f;
}

static int host_round_mode(unsigned rmode)
{
    switch (rmode) {
    case 1: return FE_UPWARD;
    case 2: return FE_DOWNWARD;
    case 3: return FE_TOWARDZERO;
    default: return FE_TONEAREST;
    }
}

// Most code leaves FPSCR at round-to-nearest, so the host rounding mode is only switched otherwise.
#define FP_ROUNDED(c, expr)                                                                                           \
    ({                                                                                                                 \
        unsigned _rm = ((c)->fpscr >> 22) & 3;                                                                         \
        float _r;                                                                                                      \
        if (likely(_rm == 0)) {                                                                                        \
            _r = (expr);                                                                                               \
        } else {                                                                                                       \
            int _old = fegetround();                                                                                   \
            fesetround(host_round_mode(_rm));                                                                          \
            _r = (expr);                                                                                               \
            fesetround(_old);                                                                                          \
        }                                                                                                              \
        _r;                                                                                                            \
    })

static void fp_compare(cpu_t *c, float a, float b, bool e)
{
    uint32_t f;
    if (isnan(a) || isnan(b)) {
        f = 0x3;
        if (e || (isnan(a) && !(f2u(a) & 0x00400000u)) || (isnan(b) && !(f2u(b) & 0x00400000u)))
            c->fpscr |= 1;
    } else if (a == b) {
        f = 0x6;
    } else if (a < b) {
        f = 0x8;
    } else {
        f = 0x2;
    }
    c->fpscr = (c->fpscr & 0x0fffffffu) | (f << 28);
}

static uint32_t fp_to_int(cpu_t *c, float f, bool is_signed, int rmode)
{
    if (isnan(f)) {
        c->fpscr |= 1;
        return 0;
    }
    double d = f;
    switch (rmode) {
    case 0: d = nearbyint(d); break;
    case 1: d = ceil(d); break;
    case 2: d = floor(d); break;
    case 3: d = trunc(d); break;
    default: d = round(d); break;
    }
    if (is_signed) {
        if (d > 2147483647.0) {
            c->fpscr |= 1;
            return 0x7fffffffu;
        }
        if (d < -2147483648.0) {
            c->fpscr |= 1;
            return 0x80000000u;
        }
        return (uint32_t)(int32_t)d;
    }
    if (d > 4294967295.0) {
        c->fpscr |= 1;
        return 0xffffffffu;
    }
    if (d < 0) {
        c->fpscr |= 1;
        return 0;
    }
    return (uint32_t)d;
}

static float round_to_integral(float f, int rmode)
{
    switch (rmode) {
    case 0: return nearbyintf(f);
    case 1: return ceilf(f);
    case 2: return floorf(f);
    case 3: return truncf(f);
    default: return roundf(f);
    }
}

static uint16_t f32_to_f16(float f)
{
    uint32_t x = f2u(f);
    uint32_t sign = (x >> 16) & 0x8000;
    int exp = (int)((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = x & 0x7fffff;
    if (((x >> 23) & 0xff) == 0xff)
        return (uint16_t)(sign | 0x7c00 | (mant ? 0x200 | (mant >> 13) : 0));
    if (exp >= 31)
        return (uint16_t)(sign | 0x7c00);
    if (exp <= 0) {
        if (exp < -10)
            return (uint16_t)sign;
        mant |= 0x800000;
        uint32_t shift = (uint32_t)(14 - exp);
        uint32_t h = mant >> shift;
        uint32_t rem = mant & ((1u << shift) - 1), halfway = 1u << (shift - 1);
        if (rem > halfway || (rem == halfway && (h & 1)))
            h++;
        return (uint16_t)(sign | h);
    }
    uint32_t h = sign | ((uint32_t)exp << 10) | (mant >> 13);
    uint32_t rem = mant & 0x1fff;
    if (rem > 0x1000 || (rem == 0x1000 && (h & 1)))
        h++;
    return (uint16_t)h;
}

static float f16_to_f32(uint16_t h)
{
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1f, mant = h & 0x3ff;
    if (exp == 0) {
        if (!mant)
            return u2f(sign);
        float f = ldexpf((float)mant, -24);
        return sign ? -f : f;
    }
    if (exp == 31)
        return u2f(sign | 0x7f800000u | (mant << 13));
    return u2f(sign | ((exp - 15 + 127) << 23) | (mant << 13));
}

static void fp_data_processing(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    unsigned d = (((hw2 >> 12) & 0xf) << 1) | ((hw1 >> 6) & 1);
    unsigned n = ((hw1 & 0xf) << 1) | ((hw2 >> 7) & 1);
    unsigned mreg = ((hw2 & 0xf) << 1) | ((hw2 >> 5) & 1);
    bool sz = hw2 & 0x100;
    bool op = hw2 & 0x40;
    unsigned o1 = ((hw1 >> 4) & 0xb);
    if (sz) {
        undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
        return;
    }
    float a = fp_in(c, s_get(c, n)), b = fp_in(c, s_get(c, mreg)), dd = fp_in(c, s_get(c, d));
    float res;
    switch (o1) {
    case 0x0: {
        float p = FP_ROUNDED(c, a * b);
        res = FP_ROUNDED(c, op ? dd - p : dd + p);
        break;
    }
    case 0x1: {
        float p = FP_ROUNDED(c, a * b);
        res = FP_ROUNDED(c, op ? -dd - p : -dd + p);
        break;
    }
    case 0x2:
        res = FP_ROUNDED(c, a * b);
        if (op)
            res = -res;
        break;
    case 0x3:
        res = FP_ROUNDED(c, op ? a - b : a + b);
        break;
    case 0x8:
        if (b == 0.0f && !isnan(a))
            c->fpscr |= 2;
        res = FP_ROUNDED(c, a / b);
        c->cyc += 13;
        break;
    case 0x9:
        res = FP_ROUNDED(c, op ? fmaf(-a, b, -dd) : fmaf(a, b, -dd));
        break;
    case 0xa:
        res = FP_ROUNDED(c, op ? fmaf(-a, b, dd) : fmaf(a, b, dd));
        break;
    case 0xb: {
        if (!op) {
            unsigned imm8 = ((hw1 & 0xf) << 4) | (hw2 & 0xf);
            uint32_t bit6 = (imm8 >> 6) & 1;
            uint32_t exp = ((bit6 ^ 1) << 7) | (bit6 ? 0x7c : 0) | ((imm8 >> 4) & 3);
            uint32_t bits = ((imm8 >> 7) << 31) | (exp << 23) | ((imm8 & 0xf) << 19);
            c->s[d] = bits;
            return;
        }
        unsigned opc2 = hw1 & 0xf;
        bool t = hw2 & 0x80;
        switch (opc2) {
        case 0x0:
            if (t)
                c->s[d] = c->s[mreg] & 0x7fffffffu;
            else
                c->s[d] = c->s[mreg];
            return;
        case 0x1:
            if (t) {
                res = FP_ROUNDED(c, sqrtf(b));
                c->cyc += 13;
                break;
            }
            c->s[d] = c->s[mreg] ^ 0x80000000u;
            return;
        case 0x2:
            s_set(c, d, f16_to_f32((uint16_t)(t ? (c->s[mreg] >> 16) : c->s[mreg])));
            return;
        case 0x3: {
            uint16_t h = f32_to_f16(b);
            if (t)
                c->s[d] = (c->s[d] & 0xffff) | ((uint32_t)h << 16);
            else
                c->s[d] = (c->s[d] & 0xffff0000u) | h;
            return;
        }
        case 0x4:
            fp_compare(c, dd, b, t);
            return;
        case 0x5:
            fp_compare(c, dd, 0.0f, t);
            return;
        case 0x6:
            res = round_to_integral(b, t ? 3 : (int)((c->fpscr >> 22) & 3));
            break;
        case 0x7:
            if (!t) {
                res = round_to_integral(b, (int)((c->fpscr >> 22) & 3));
                if (res != b)
                    c->fpscr |= 1u << 4;
                break;
            }
            undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
            return;
        case 0x8:
            if (t)
                res = FP_ROUNDED(c, (float)(int32_t)c->s[mreg]);
            else
                res = FP_ROUNDED(c, (float)c->s[mreg]);
            break;
        case 0xa:
        case 0xb:
        case 0xe:
        case 0xf: {
            bool to_fixed = opc2 & 4, uns = opc2 & 1, sx = hw2 & 0x80;
            unsigned size = sx ? 32 : 16;
            unsigned imm = ((hw2 & 0xf) << 1) | ((hw2 >> 5) & 1);
            int frac = (int)size - (int)imm;
            if (to_fixed) {
                float f = ldexpf(s_get(c, d), frac);
                uint32_t v = fp_to_int(c, f, !uns, 3);
                if (size == 16) {
                    if (uns)
                        v = (float)v > 65535.0f ? 0xffff : v;
                    else {
                        int32_t sv = (int32_t)v;
                        v = (uint32_t)(sv > 32767 ? 32767 : sv < -32768 ? -32768 : sv) & 0xffff;
                        if (sv < 0)
                            v |= 0xffff0000u;
                    }
                }
                c->s[d] = v;
            } else {
                uint32_t raw = c->s[d];
                double v;
                if (size == 16)
                    v = uns ? (double)(raw & 0xffff) : (double)(int16_t)raw;
                else
                    v = uns ? (double)raw : (double)(int32_t)raw;
                s_set(c, d, FP_ROUNDED(c, (float)ldexp(v, -frac)));
            }
            return;
        }
        case 0xc:
        case 0xd: {
            bool is_signed = opc2 & 1;
            c->s[d] = fp_to_int(c, b, is_signed, t ? 3 : (int)((c->fpscr >> 22) & 3));
            return;
        }
        default:
            undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
            return;
        }
        break;
    }
    default:
        undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
        return;
    }
    s_set(c, d, fp_out(c, res));
}

static void fp_v8_extension(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    unsigned d = (((hw2 >> 12) & 0xf) << 1) | ((hw1 >> 6) & 1);
    unsigned n = ((hw1 & 0xf) << 1) | ((hw2 >> 7) & 1);
    unsigned mreg = ((hw2 & 0xf) << 1) | ((hw2 >> 5) & 1);
    if (hw2 & 0x100) {
        undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
        return;
    }
    if ((hw1 & 0xff80) == 0xfe00 && !(hw2 & 0x50)) {
        bool cond;
        uint32_t f = c->apsr;
        bool z = f & F_Z, nn = f & F_N, v = f & F_V;
        switch ((hw1 >> 4) & 3) {
        case 0: cond = z; break;
        case 1: cond = v; break;
        case 2: cond = nn == v; break;
        default: cond = !z && nn == v; break;
        }
        c->s[d] = cond ? c->s[n] : c->s[mreg];
        return;
    }
    if ((hw1 & 0xffb0) == 0xfe80 && !(hw2 & 0x10)) {
        float a = s_get(c, n), b = s_get(c, mreg);
        s_set(c, d, (hw2 & 0x40) ? fminf(a, b) : fmaxf(a, b));
        return;
    }
    if ((hw1 & 0xffbc) == 0xfeb8 && (hw2 & 0xc0) == 0x40) {
        static const int modes[4] = {4, 0, 1, 2};
        s_set(c, d, round_to_integral(s_get(c, mreg), modes[hw1 & 3]));
        return;
    }
    if ((hw1 & 0xffbc) == 0xfebc && (hw2 & 0x40)) {
        static const int modes[4] = {4, 0, 1, 2};
        c->s[d] = fp_to_int(c, s_get(c, mreg), hw2 & 0x80, modes[hw1 & 3]);
        return;
    }
    undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
}

static void fp_transfer(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    uint32_t *r = c->r;
    unsigned rt = (hw2 >> 12) & 0xf;
    unsigned a = (hw1 >> 5) & 7;
    bool l = hw1 & 0x10;
    if (!(hw2 & 0x100)) {
        if (a == 0) {
            unsigned n = ((hw1 & 0xf) << 1) | ((hw2 >> 7) & 1);
            if (l)
                r[rt] = c->s[n];
            else
                c->s[n] = r[rt];
            return;
        }
        if (a == 7) {
            if (l) {
                uint32_t v = (hw1 & 0xf) == 1 ? c->fpscr : 0;
                if (rt == 15)
                    c->apsr = (c->apsr & 0x0fffffffu) | (v & 0xf0000000u);
                else
                    r[rt] = v;
            } else if ((hw1 & 0xf) == 1) {
                c->fpscr = r[rt] & 0xf7c0009fu;
            }
            return;
        }
    } else if ((hw2 & 0x6f) == 0) {
        unsigned idx = (hw1 >> 5) & 1;
        unsigned dreg = ((hw2 >> 7) & 1) << 4 | (hw1 & 0xf);
        unsigned s = dreg * 2 + idx;
        if (s < 32) {
            if (l)
                r[rt] = c->s[s];
            else
                c->s[s] = r[rt];
            return;
        }
    }
    undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
}

static void fp_load_store(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    uint32_t *r = c->r;
    unsigned rn = hw1 & 0xf;
    bool sz = hw2 & 0x100;
    if ((hw1 & 0x0fe0) == 0x0c40) {
        unsigned rt = (hw2 >> 12) & 0xf, rt2 = hw1 & 0xf;
        unsigned mreg = sz ? ((((hw2 >> 5) & 1) << 4) | (hw2 & 0xf)) * 2 : (((hw2 & 0xf) << 1) | ((hw2 >> 5) & 1));
        if (mreg > 30) {
            undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
            return;
        }
        if (hw1 & 0x10) {
            r[rt] = c->s[mreg];
            r[rt2] = c->s[mreg + 1];
        } else {
            c->s[mreg] = r[rt];
            c->s[mreg + 1] = r[rt2];
        }
        return;
    }
    bool p = hw1 & 0x100, u = hw1 & 0x80, w = hw1 & 0x20, l = hw1 & 0x10;
    unsigned imm8 = hw2 & 0xff;
    unsigned dreg = sz ? (((hw1 >> 6) & 1) << 4 | ((hw2 >> 12) & 0xf)) * 2 : ((((hw2 >> 12) & 0xf) << 1) | ((hw1 >> 6) & 1));
    if (p && !w) {
        uint32_t base = rn == 15 ? ((pc + 4) & ~3u) : r[rn];
        uint32_t a = u ? base + imm8 * 4 : base - imm8 * 4;
        unsigned words = sz ? 2 : 1;
        for (unsigned i = 0; i < words; i++) {
            if (l)
                c->s[(dreg + i) & 31] = ld32(c, a + 4 * i);
            else
                st32(c, a + 4 * i, c->s[(dreg + i) & 31]);
        }
        return;
    }
    if (p == u) {
        undefined(c, pc, ((uint32_t)hw1 << 16) | hw2);
        return;
    }
    unsigned words = imm8;
    uint32_t base = r[rn];
    uint32_t a = p ? base - 4 * words : base;
    for (unsigned i = 0; i < words; i++) {
        unsigned s = (dreg + i) & 31;
        if (l)
            c->s[s] = ld32(c, a + 4 * i);
        else
            st32(c, a + 4 * i, c->s[s]);
    }
    if (w && !c->fault)
        r[rn] = p ? base - 4 * words : base + 4 * words;
}

static void exec_fp(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    c->control |= 4;
    if ((hw1 & 0xff00) == 0xfe00) {
        fp_v8_extension(c, pc, hw1, hw2);
    } else if ((hw1 & 0x0f00) == 0x0e00) {
        if (hw2 & 0x10)
            fp_transfer(c, pc, hw1, hw2);
        else
            fp_data_processing(c, pc, hw1, hw2);
    } else {
        fp_load_store(c, pc, hw1, hw2);
    }
}
