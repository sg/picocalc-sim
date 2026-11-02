#include <math.h>
#include <string.h>

#include "chip.h"

// The RP2350's double-precision coprocessor, modelled by what each canned sequence computes
// rather than by its internal datapath: X and Y are captured as doubles when written unpacked,
// the steps where the CPU refines a mantissa are accepted and ignored, and the final read returns
// the correctly rounded IEEE result the hardware produces.

enum { OP_NONE, OP_ADD, OP_SUB, OP_SQRT };

static inline double bits_to_d(uint64_t b)
{
    double d;
    memcpy(&d, &b, 8);
    return d;
}

static inline uint64_t d_to_bits(double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    return b;
}

static inline float bits_to_f(uint32_t b)
{
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static inline uint32_t f_to_bits(float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

typedef struct {
    uint64_t x, y;
    int op;
    int round;
    bool engaged;
} dcp_state_t;

static dcp_state_t st[2];

static double X(dcp_state_t *s)
{
    return bits_to_d(s->x);
}

static double Y(dcp_state_t *s)
{
    return bits_to_d(s->y);
}

bool dcp_mcrr(cpu_t *c, unsigned opc1, unsigned crm, uint32_t lo, uint32_t hi)
{
    dcp_state_t *s = &st[c->id];
    uint64_t v = ((uint64_t)hi << 32) | lo;
    switch (opc1) {
    case 0:
        if (crm == 0)
            s->x = v;
        else if (crm == 1)
            s->y = v;
        else {
            s->op = (int)(lo & 0xff);
            s->round = (int)((lo >> 8) & 0xff);
            s->engaged = hi & 1;
        }
        return true;
    case 1:
        if (crm == 0)
            s->x = v;
        else if (crm == 1)
            s->y = v;
        else {
            s->x = d_to_bits((double)bits_to_f(lo));
            s->y = d_to_bits((double)bits_to_f(hi));
        }
        s->engaged = true;
        s->op = OP_NONE;
        return true;
    case 6:
        s->x = d_to_bits((double)lo);
        s->y = 0;
        s->engaged = true;
        return true;
    case 7:
        s->x = d_to_bits((double)(int32_t)lo);
        s->y = 0;
        s->engaged = true;
        return true;
    case 8:
        s->x = v;
        s->y = 0;
        s->engaged = true;
        return true;
    case 9:
        s->x = d_to_bits((double)bits_to_f(lo));
        s->y = 0;
        s->engaged = true;
        return true;
    case 2:
    case 3:
    case 4:
    case 5:
    case 10:
    case 11:
    case 12:
        return true;
    default:
        return false;
    }
}

bool dcp_cdp(cpu_t *c, unsigned opc1, unsigned crd, unsigned crn, unsigned crm, unsigned opc2)
{
    dcp_state_t *s = &st[c->id];
    (void)crd;
    (void)crn;
    switch (opc1) {
    case 0:
        if (crm == 0)
            memset(s, 0, sizeof(*s));
        else if (s->op == OP_NONE)
            s->op = OP_ADD;
        return true;
    case 1:
        s->op = opc2 == 1 ? OP_SUB : OP_ADD;
        return true;
    case 2:
        s->op = OP_SQRT;
        return true;
    case 8:
        s->round = (int)opc2;
        return true;
    default:
        return false;
    }
}

static uint32_t compare_flags(dcp_state_t *s)
{
    double x = X(s), y = Y(s);
    if (isnan(x) || isnan(y))
        return 0x10000000u;
    if (x == y)
        return 0x60000000u;
    if (x < y)
        return 0x80000000u;
    return 0x20000000u;
}

static uint32_t to_int(double d, bool is_signed, bool round_nearest)
{
    if (isnan(d))
        return 0;
    d = round_nearest ? nearbyint(d) : trunc(d);
    if (is_signed) {
        if (d >= 2147483647.0)
            return 0x7fffffffu;
        if (d <= -2147483648.0)
            return 0x80000000u;
        return (uint32_t)(int32_t)d;
    }
    if (d >= 4294967295.0)
        return 0xffffffffu;
    if (d <= 0)
        return 0;
    return (uint32_t)d;
}

bool dcp_mrc(cpu_t *c, unsigned crm, unsigned opc2, uint32_t *v, bool pop)
{
    dcp_state_t *s = &st[c->id];
    switch (crm) {
    case 0:
        if (opc2 == 1) {
            if (pop) {
                *v = s->engaged ? 0x80000000u : 0;
                return true;
            }
            *v = compare_flags(s);
            s->engaged = false;
            return true;
        }
        *v = 0;
        return true;
    case 2: {
        double x = X(s), y = Y(s), r;
        switch (opc2) {
        case 0: r = x + y; break;
        case 1: r = x - y; break;
        case 2: r = x * y; break;
        case 3: r = x / y; break;
        case 4: r = sqrt(x); break;
        default: r = x; break;
        }
        *v = f_to_bits((float)r);
        s->engaged = false;
        return true;
    }
    case 3:
        *v = to_int(X(s), opc2 == 0, s->round == 3);
        s->engaged = false;
        return true;
    default:
        return false;
    }
}

bool dcp_mrrc(cpu_t *c, unsigned opc1, unsigned crm, uint32_t *lo, uint32_t *hi, bool pop)
{
    dcp_state_t *s = &st[c->id];
    (void)pop;
    uint64_t r;
    switch (crm) {
    case 8: r = s->x; break;
    case 9: r = s->y; break;
    case 10: r = (uint64_t)(uint32_t)(s->op | (s->round << 8)) | ((uint64_t)s->engaged << 32); break;
    case 4:
    case 5: {
        uint64_t b = crm == 4 ? s->x : s->y;
        r = ((b & 0x000fffffffffffffull) | 0x0010000000000000ull) << (opc1 & 7);
        break;
    }
    case 1: {
        double y = Y(s);
        r = d_to_bits(y != 0 ? 1.0 / y : 0);
        break;
    }
    case 0: {
        double x = X(s), y = Y(s), res;
        switch (opc1) {
        case 1: res = s->op == OP_SUB ? x - y : x + y; break;
        case 3: res = x - y; break;
        case 5: res = x * y; break;
        case 7: res = x / y; break;
        case 9: res = sqrt(x); break;
        default: res = x; break;
        }
        r = d_to_bits(res);
        s->engaged = false;
        break;
    }
    default:
        return false;
    }
    *lo = (uint32_t)r;
    *hi = (uint32_t)(r >> 32);
    return true;
}
