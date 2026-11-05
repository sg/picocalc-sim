// Predecoded blocks: straight-line runs of instructions decoded once and then dispatched straight
// to small handlers, which skips the fetch and the decode switch for each instruction. Every
// handler does exactly what exec16 or exec32 does for its instruction, cycles included, so the
// emulation is the same as stepping one instruction at a time.
//
// A block from ROM or flash is used only while the chip's code generation is unchanged, which
// flash programming, remapping and resets advance. One from SRAM or PSRAM, which ordinary stores
// and DMA can change, keeps a copy of its code and is used only while memory still holds those
// bytes. Blocks run only outside IT blocks, so the 16-bit data-processing forms always set the
// flags here. A block may end with an IT instruction and the instructions it covers, but block_run
// stops at the IT instruction and leaves the rest to cpu_run's stepping; only native code runs them.

#define BLK_MAX 16
#define BLK_SETS 4096

typedef struct dop dop_t;
typedef void (*dop_fn)(cpu_t *c, const dop_t *d);

// lf holds the instruction's length in bytes and whether its first or second halfword's fetch may
// need a new XIP cache line.
#define LF_LEN 7
#define FETCH_HW1 8
#define FETCH_HW2 16
// Spare lf bits that some handlers use for flags they decoded.
#define LF_A 32
#define LF_B 64
#define LF_C 128

struct dop {
    dop_fn fn;
    uint32_t pc;
    uint16_t hw1, hw2;
    uint8_t lf, rd, rn, rm;
    uint32_t imm;
};

typedef struct block block_t;
struct block {
    uint32_t tag; // start address | 1, so an empty entry never matches
    uint32_t gen; // the chip's code_gen when built, which is all a block from ROM or flash checks
    uint16_t n, bytes;
    bool fixed; // from ROM or flash
    uint8_t nrun; // how many of the ops block_run may run: up to an IT instruction, if any
    // The blocks control went on to from here, fall-through first, checked like any other.
    uint32_t link_pc[2];
    block_t *link[2];
    // Native code for the block and the translation cache epoch it belongs to.
    void *jit;
    uint32_t jit_epoch;
    dop_t ops[BLK_MAX];
    uint8_t code[BLK_MAX * 4];
};

// Two ways per set, shared by both cores, which run on one thread.
static block_t blocks[BLK_SETS][2];
static uint8_t block_mru[BLK_SETS];
static int blocks_enabled = -1;

// ---------------------------------------------------------------------------------------------
// Handlers

static void h_exec16(cpu_t *c, const dop_t *d)
{
    exec16(c, d->pc, d->hw1);
}

static void h_exec32(cpu_t *c, const dop_t *d)
{
    exec32(c, d->pc, d->hw1, d->hw2);
}

static void h_ldst_dual_excl(cpu_t *c, const dop_t *d)
{
    ldst_dual_excl(c, d->pc, d->hw1, d->hw2);
}

static void h_dp_shifted_reg(cpu_t *c, const dop_t *d)
{
    dp_shifted_reg(c, d->pc, d->hw1, d->hw2);
}

static void h_branch_misc(cpu_t *c, const dop_t *d)
{
    branch_misc(c, d->pc, d->hw1, d->hw2);
}

static void h_dp_plain_imm(cpu_t *c, const dop_t *d)
{
    dp_plain_imm(c, d->pc, d->hw1, d->hw2);
}

static void h_ldst_single(cpu_t *c, const dop_t *d)
{
    ldst_single(c, d->pc, d->hw1, d->hw2);
}

static void h_dp_register(cpu_t *c, const dop_t *d)
{
    dp_register(c, d->pc, d->hw1, d->hw2);
}

static void h_multiply(cpu_t *c, const dop_t *d)
{
    multiply(c, d->pc, d->hw1, d->hw2);
}

static void h_long_multiply(cpu_t *c, const dop_t *d)
{
    long_multiply(c, d->pc, d->hw1, d->hw2);
}

static void h_nop(cpu_t *c, const dop_t *d)
{
}

static void h_movs_reg(cpu_t *c, const dop_t *d)
{
    uint32_t v = c->r[d->rm];
    c->r[d->rd] = v;
    set_nz(c, v);
}

static void h_lsl_imm(cpu_t *c, const dop_t *d)
{
    uint32_t v = c->r[d->rm];
    c->r[d->rd] = v << d->imm;
    set_nzc(c, c->r[d->rd], (v >> (32 - d->imm)) & 1);
}

static void h_lsr_imm(cpu_t *c, const dop_t *d)
{
    uint32_t co;
    c->r[d->rd] = imm_shift_c(c->r[d->rm], 1, d->imm, 0, &co);
    set_nzc(c, c->r[d->rd], co);
}

static void h_asr_imm(cpu_t *c, const dop_t *d)
{
    uint32_t co;
    c->r[d->rd] = imm_shift_c(c->r[d->rm], 2, d->imm, 0, &co);
    set_nzc(c, c->r[d->rd], co);
}

static void h_add_reg(cpu_t *c, const dop_t *d)
{
    c->r[d->rd] = add_c(c, c->r[d->rn], c->r[d->rm], 0, true);
}

static void h_sub_reg(cpu_t *c, const dop_t *d)
{
    c->r[d->rd] = add_c(c, c->r[d->rn], ~c->r[d->rm], 1, true);
}

static void h_add_imm(cpu_t *c, const dop_t *d)
{
    c->r[d->rd] = add_c(c, c->r[d->rn], d->imm, 0, true);
}

static void h_sub_imm(cpu_t *c, const dop_t *d)
{
    c->r[d->rd] = add_c(c, c->r[d->rn], ~d->imm, 1, true);
}

static void h_movs_imm(cpu_t *c, const dop_t *d)
{
    c->r[d->rd] = d->imm;
    set_nz(c, d->imm);
}

static void h_cmp_imm(cpu_t *c, const dop_t *d)
{
    add_c(c, c->r[d->rn], ~d->imm, 1, true);
}

static void h_cmp_reg(cpu_t *c, const dop_t *d)
{
    add_c(c, c->r[d->rn], ~c->r[d->rm], 1, true);
}

static void h_alu(cpu_t *c, const dop_t *d)
{
    uint32_t *r = c->r;
    unsigned rdn = d->rd;
    uint32_t a = r[rdn], b = r[d->rm], co;
    uint32_t cin = (c->apsr & F_C) ? 1 : 0;
    switch (d->imm) {
    case 0x0: r[rdn] = a & b; set_nz(c, r[rdn]); break;
    case 0x1: r[rdn] = a ^ b; set_nz(c, r[rdn]); break;
    case 0x2: r[rdn] = shift_c(a, 0, b & 0xff, cin, &co); set_nzc(c, r[rdn], co); break;
    case 0x3: r[rdn] = shift_c(a, 1, b & 0xff, cin, &co); set_nzc(c, r[rdn], co); break;
    case 0x4: r[rdn] = shift_c(a, 2, b & 0xff, cin, &co); set_nzc(c, r[rdn], co); break;
    case 0x5: r[rdn] = add_c(c, a, b, cin, true); break;
    case 0x6: r[rdn] = add_c(c, a, ~b, cin, true); break;
    case 0x7: r[rdn] = shift_c(a, 3, b & 0xff, cin, &co); set_nzc(c, r[rdn], co); break;
    case 0x8: set_nz(c, a & b); break;
    case 0x9: r[rdn] = add_c(c, 0, ~b, 1, true); break;
    case 0xa: add_c(c, a, ~b, 1, true); break;
    case 0xb: add_c(c, a, b, 0, true); break;
    case 0xc: r[rdn] = a | b; set_nz(c, r[rdn]); break;
    case 0xd: r[rdn] = a * b; set_nz(c, r[rdn]); break;
    case 0xe: r[rdn] = a & ~b; set_nz(c, r[rdn]); break;
    case 0xf: r[rdn] = ~b; set_nz(c, r[rdn]); break;
    }
}

static void h_add_hi(cpu_t *c, const dop_t *d)
{
    c->r[d->rd] += c->r[d->rm];
}

static void h_mov_hi(cpu_t *c, const dop_t *d)
{
    c->r[d->rd] = c->r[d->rm];
}

static void h_ldr_lit(cpu_t *c, const dop_t *d)
{
    c->r[d->rd] = ld32(c, d->imm);
}

static void h_ldst_reg(cpu_t *c, const dop_t *d)
{
    uint32_t *r = c->r;
    unsigned rt = d->rd;
    uint32_t a = r[d->rn] + r[d->rm];
    switch (d->imm) {
    case 0: st32u(c, a, r[rt]); break;
    case 1: st16u(c, a, r[rt]); break;
    case 2: st8(c, a, r[rt]); break;
    case 3: r[rt] = (uint32_t)(int32_t)(int8_t)ld8(c, a); break;
    case 4: r[rt] = ld32u(c, a); break;
    case 5: r[rt] = ld16u(c, a); break;
    case 6: r[rt] = ld8(c, a); break;
    case 7: r[rt] = (uint32_t)(int32_t)(int16_t)ld16u(c, a); break;
    }
}

static void h_str_imm(cpu_t *c, const dop_t *d)
{
    st32u(c, c->r[d->rn] + d->imm, c->r[d->rd]);
}

static void h_ldr_imm(cpu_t *c, const dop_t *d)
{
    c->r[d->rd] = ld32u(c, c->r[d->rn] + d->imm);
}

static void h_strb_imm(cpu_t *c, const dop_t *d)
{
    st8(c, c->r[d->rn] + d->imm, c->r[d->rd]);
}

static void h_ldrb_imm(cpu_t *c, const dop_t *d)
{
    c->r[d->rd] = ld8(c, c->r[d->rn] + d->imm);
}

static void h_strh_imm(cpu_t *c, const dop_t *d)
{
    st16u(c, c->r[d->rn] + d->imm, c->r[d->rd]);
}

static void h_ldrh_imm(cpu_t *c, const dop_t *d)
{
    c->r[d->rd] = ld16u(c, c->r[d->rn] + d->imm);
}

static void h_set_const(cpu_t *c, const dop_t *d)
{
    c->r[d->rd] = d->imm;
}

static void h_add_sp_imm(cpu_t *c, const dop_t *d)
{
    c->r[d->rd] = c->r[13] + d->imm;
}

static void h_adjust_sp(cpu_t *c, const dop_t *d)
{
    c->r[13] += d->imm;
}

static void h_extend(cpu_t *c, const dop_t *d)
{
    uint32_t v = c->r[d->rm];
    switch (d->imm) {
    case 0: c->r[d->rd] = (uint32_t)(int32_t)(int16_t)v; break;
    case 1: c->r[d->rd] = (uint32_t)(int32_t)(int8_t)v; break;
    case 2: c->r[d->rd] = v & 0xffff; break;
    case 3: c->r[d->rd] = v & 0xff; break;
    }
}

static void h_cbz(cpu_t *c, const dop_t *d)
{
    if ((c->r[d->rn] != 0) == (d->rd != 0)) {
        c->r[15] = d->imm;
        c->cyc++;
    }
}

static void h_push(cpu_t *c, const dop_t *d)
{
    uint32_t *r = c->r;
    uint32_t list = d->imm;
    uint32_t a = r[13] - 4 * d->rm;
    uint32_t sp = a;
    for (int i = 0; i < 15; i++)
        if (list & (1u << i)) {
            st32(c, a, r[i]);
            a += 4;
        }
    if (!c->fault)
        r[13] = sp;
}

static void h_pop(cpu_t *c, const dop_t *d)
{
    uint32_t *r = c->r;
    uint32_t list = d->imm & 0xff;
    uint32_t a = r[13];
    uint32_t vals[8];
    for (int i = 0; i < 8; i++)
        if (list & (1u << i)) {
            vals[i] = ld32(c, a);
            a += 4;
        }
    uint32_t newpc = 0;
    if (d->imm & 0x100) {
        newpc = ld32(c, a);
        a += 4;
    }
    if (c->fault)
        return;
    for (int i = 0; i < 8; i++)
        if (list & (1u << i))
            r[i] = vals[i];
    r[13] = a;
    if (d->imm & 0x100) {
        c->cyc += 2;
        branch_interworking(c, newpc, d->pc);
    }
}

static void h_bx(cpu_t *c, const dop_t *d)
{
    uint32_t v = c->r[d->rm];
    c->cyc++;
    branch_interworking(c, v, d->pc);
}

static void h_blx(cpu_t *c, const dop_t *d)
{
    uint32_t v = c->r[d->rm];
    c->cyc++;
    c->r[14] = (d->pc + 2) | 1;
    branch_interworking(c, v, d->pc);
}

static void h_bl(cpu_t *c, const dop_t *d)
{
    c->r[14] = (d->pc + 4) | 1;
    c->cyc += 2;
    c->r[15] = d->imm;
}

// Modified-immediate data processing: LF_A sets the flags; LF_B marks a rotated constant, whose
// carry out is its top bit rather than the carry flag.
static void h_dp_imm(cpu_t *c, const dop_t *d)
{
    uint32_t carry = (d->lf & LF_B) ? d->imm >> 31 : ((c->apsr & F_C) ? 1 : 0);
    dp_op(c, d->rm, d->rd, d->rn, (d->lf & LF_A) != 0, c->r[d->rn], d->imm, carry, d->pc);
}

// LDM/STM with the list (already masked for a store) in imm and its length in rm; LF_A loads,
// LF_B writes back, LF_C decrements before.
static void h_ldm_stm(cpu_t *c, const dop_t *d)
{
    uint32_t *r = c->r;
    unsigned rn = d->rn, n = d->rm;
    uint32_t list = d->imm;
    bool load = d->lf & LF_A, wb = d->lf & LF_B, db = d->lf & LF_C;
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
            branch_interworking(c, vals[15], d->pc);
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

// The 32-bit loads write their register only once the access has not faulted.
static void h_ldr_w(cpu_t *c, const dop_t *d)
{
    uint32_t v = ld32u(c, c->r[d->rn] + d->imm);
    if (!c->fault)
        c->r[d->rd] = v;
}

static void h_ldrb_w(cpu_t *c, const dop_t *d)
{
    uint32_t v = ld8(c, c->r[d->rn] + d->imm);
    if (!c->fault)
        c->r[d->rd] = v;
}

static void h_ldrh_w(cpu_t *c, const dop_t *d)
{
    uint32_t v = ld16u(c, c->r[d->rn] + d->imm);
    if (!c->fault)
        c->r[d->rd] = v;
}

// LDR/STR with an 8-bit offset, signed in imm: LF_A indexes before the access, LF_B writes back.
static void h_ldr_w8(cpu_t *c, const dop_t *d)
{
    uint32_t *r = c->r;
    uint32_t off = r[d->rn] + d->imm;
    uint32_t v = ld32u(c, (d->lf & LF_A) ? off : r[d->rn]);
    if (c->fault)
        return;
    if (d->lf & LF_B)
        r[d->rn] = off;
    r[d->rd] = v;
}

static void h_str_w8(cpu_t *c, const dop_t *d)
{
    uint32_t *r = c->r;
    uint32_t off = r[d->rn] + d->imm;
    st32u(c, (d->lf & LF_A) ? off : r[d->rn], r[d->rd]);
    if ((d->lf & LF_B) && !c->fault)
        r[d->rn] = off;
}

// LDRD/STRD with rt2 in rm and the signed offset in imm: LF_A indexes before the access, LF_B
// writes back, LF_C loads.
static void h_ldrd_strd(cpu_t *c, const dop_t *d)
{
    uint32_t *r = c->r;
    uint32_t base = r[d->rn];
    uint32_t offaddr = base + d->imm;
    uint32_t a = (d->lf & LF_A) ? offaddr : base;
    if (d->lf & LF_C) {
        uint32_t v1 = ld32(c, a), v2 = ld32(c, a + 4);
        if (c->fault)
            return;
        r[d->rd] = v1;
        r[d->rm] = v2;
    } else {
        st32(c, a, r[d->rd]);
        st32(c, a + 4, r[d->rm]);
    }
    if ((d->lf & LF_B) && !c->fault)
        r[d->rn] = offaddr;
}

static void h_bcond(cpu_t *c, const dop_t *d)
{
    if (cond_pass(c, d->rd)) {
        c->r[15] = d->imm;
        c->cyc++;
    }
}

static void h_branch(cpu_t *c, const dop_t *d)
{
    c->r[15] = d->imm;
    c->cyc++;
}

// ---------------------------------------------------------------------------------------------
// Decoding

// Fills in d for one instruction and returns whether it ends the block: anything that branches,
// starts an IT block or changes the core's state.
static bool decode16(dop_t *d, uint32_t pc, uint16_t op)
{
    d->fn = h_exec16;
    unsigned lo3 = op & 7, mid3 = (op >> 3) & 7, imm5 = (op >> 6) & 31, hi3 = (op >> 8) & 7;
    switch (op >> 11) {
    case 0x00:
        d->rd = lo3, d->rm = mid3, d->imm = imm5;
        d->fn = imm5 ? h_lsl_imm : h_movs_reg;
        return false;
    case 0x01:
        d->rd = lo3, d->rm = mid3, d->imm = imm5;
        d->fn = h_lsr_imm;
        return false;
    case 0x02:
        d->rd = lo3, d->rm = mid3, d->imm = imm5;
        d->fn = h_asr_imm;
        return false;
    case 0x03:
        d->rd = lo3, d->rn = mid3, d->rm = (op >> 6) & 7, d->imm = (op >> 6) & 7;
        if (op & 0x400)
            d->fn = (op & 0x200) ? h_sub_imm : h_add_imm;
        else
            d->fn = (op & 0x200) ? h_sub_reg : h_add_reg;
        return false;
    case 0x04:
        d->rd = hi3, d->imm = op & 0xff;
        d->fn = h_movs_imm;
        return false;
    case 0x05:
        d->rn = hi3, d->imm = op & 0xff;
        d->fn = h_cmp_imm;
        return false;
    case 0x06:
        d->rd = d->rn = hi3, d->imm = op & 0xff;
        d->fn = h_add_imm;
        return false;
    case 0x07:
        d->rd = d->rn = hi3, d->imm = op & 0xff;
        d->fn = h_sub_imm;
        return false;
    case 0x08:
        if (!(op & 0x400)) {
            d->rd = d->rn = lo3, d->rm = mid3, d->imm = (op >> 6) & 0xf;
            d->fn = d->imm == 0xa ? h_cmp_reg : h_alu;
            return false;
        } else {
            unsigned rm = (op >> 3) & 0xf, rdn = (op & 7) | ((op >> 4) & 8);
            unsigned kind = (op >> 8) & 3;
            d->rd = d->rn = rdn, d->rm = rm;
            if (kind == 3) {
                if (rm != 15 && !(op & 4))
                    d->fn = (op & 0x80) ? h_blx : h_bx;
                return true;
            }
            if (rdn == 15)
                return true;
            if (rm != 15)
                d->fn = kind == 0 ? h_add_hi : kind == 1 ? h_cmp_reg : h_mov_hi;
            return false;
        }
    case 0x09:
        d->rd = hi3, d->imm = ((pc + 4) & ~3u) + ((op & 0xff) << 2);
        d->fn = h_ldr_lit;
        return false;
    case 0x0a:
    case 0x0b:
        d->rd = lo3, d->rn = mid3, d->rm = (op >> 6) & 7, d->imm = (op >> 9) & 7;
        d->fn = h_ldst_reg;
        return false;
    case 0x0c:
        d->rd = lo3, d->rn = mid3, d->imm = imm5 << 2;
        d->fn = h_str_imm;
        return false;
    case 0x0d:
        d->rd = lo3, d->rn = mid3, d->imm = imm5 << 2;
        d->fn = h_ldr_imm;
        return false;
    case 0x0e:
        d->rd = lo3, d->rn = mid3, d->imm = imm5;
        d->fn = h_strb_imm;
        return false;
    case 0x0f:
        d->rd = lo3, d->rn = mid3, d->imm = imm5;
        d->fn = h_ldrb_imm;
        return false;
    case 0x10:
        d->rd = lo3, d->rn = mid3, d->imm = imm5 << 1;
        d->fn = h_strh_imm;
        return false;
    case 0x11:
        d->rd = lo3, d->rn = mid3, d->imm = imm5 << 1;
        d->fn = h_ldrh_imm;
        return false;
    case 0x12:
        d->rd = hi3, d->rn = 13, d->imm = (op & 0xff) << 2;
        d->fn = h_str_imm;
        return false;
    case 0x13:
        d->rd = hi3, d->rn = 13, d->imm = (op & 0xff) << 2;
        d->fn = h_ldr_imm;
        return false;
    case 0x14:
        d->rd = hi3, d->imm = ((pc + 4) & ~3u) + ((op & 0xff) << 2);
        d->fn = h_set_const;
        return false;
    case 0x15:
        d->rd = hi3, d->imm = (op & 0xff) << 2;
        d->fn = h_add_sp_imm;
        return false;
    case 0x16:
    case 0x17:
        if ((op & 0xf500) == 0xb100) {
            d->rn = lo3, d->rd = (op & 0x800) != 0;
            d->imm = pc + 4 + ((((op >> 9) & 1) << 6) | (((op >> 3) & 31) << 1));
            d->fn = h_cbz;
            return true;
        }
        switch ((op >> 8) & 0xf) {
        case 0x0:
            d->imm = (op & 0x80) ? 0u - ((op & 0x7fu) << 2) : (op & 0x7fu) << 2;
            d->fn = h_adjust_sp;
            return false;
        case 0x2:
            d->rd = lo3, d->rm = mid3, d->imm = (op >> 6) & 3;
            d->fn = h_extend;
            return false;
        case 0x4:
        case 0x5:
            d->imm = (op & 0xff) | ((op & 0x100) ? 0x4000 : 0);
            d->rm = (uint8_t)__builtin_popcount(d->imm);
            d->fn = h_push;
            return false;
        case 0xa:
            return false;
        case 0xc:
        case 0xd:
            d->imm = op & 0x1ff;
            d->fn = h_pop;
            return (op & 0x100) != 0;
        case 0xf: {
            // NOP, YIELD and the unallocated hints do nothing; WFE, WFI, SEV and IT end the block.
            unsigned hint = (op >> 4) & 0xf;
            if (!(op & 0xf) && (hint < 2 || hint > 4)) {
                d->fn = h_nop;
                return false;
            }
            return true;
        }
        default:
            return true;
        }
    case 0x18:
    case 0x19:
        return false;
    case 0x1a:
    case 0x1b: {
        unsigned cond = (op >> 8) & 0xf;
        if (cond < 0xe) {
            d->rd = cond, d->imm = pc + 4 + ((uint32_t)(int32_t)(int8_t)(op & 0xff) << 1);
            d->fn = h_bcond;
        }
        return true;
    }
    case 0x1c: {
        uint32_t imm = (uint32_t)(op & 0x7ff) << 1;
        if (imm & 0x800)
            imm |= 0xfffff000u;
        d->imm = pc + 4 + imm;
        d->fn = h_branch;
        return true;
    }
    default:
        return true;
    }
}

static void decode_ldm_stm(dop_t *d, uint16_t hw1, uint16_t hw2)
{
    bool load = hw1 & 0x10;
    uint32_t list = load ? hw2 : hw2 & 0x5fffu;
    d->rn = hw1 & 0xf;
    d->imm = list;
    d->rm = (uint8_t)__builtin_popcount(list);
    d->lf |= (load ? LF_A : 0) | ((hw1 & 0x20) ? LF_B : 0) | ((hw1 & 0x180) == 0x100 ? LF_C : 0);
    d->fn = h_ldm_stm;
}

// The doubleword forms of ldst_dual_excl, which come after its exclusive, TT, SG and table
// branch encodings.
static bool decode_ldrd_strd(dop_t *d, uint16_t hw1, uint16_t hw2)
{
    if ((hw1 & 0xffe0) == 0xe840 || (hw1 & 0xffe0) == 0xe8c0 || (hw1 == 0xe97f && hw2 == 0xe97f) ||
        (hw1 & 0xf) == 15)
        return false;
    unsigned op = (hw1 >> 4) & 0x1f;
    uint32_t imm = (hw2 & 0xffu) << 2;
    d->rn = hw1 & 0xf, d->rd = (hw2 >> 12) & 0xf, d->rm = (hw2 >> 8) & 0xf;
    d->imm = (op & 0x08) ? imm : 0u - imm;
    d->lf |= ((op & 0x10) ? LF_A : 0) | ((op & 0x02) ? LF_B : 0) | ((op & 0x01) ? LF_C : 0);
    d->fn = h_ldrd_strd;
    return true;
}

// Loads and stores with an immediate offset from a base other than the PC, into a register other
// than the PC; anything else stays with ldst_single.
static bool decode_ldst_single(dop_t *d, uint16_t hw1, uint16_t hw2)
{
    bool sign = hw1 & 0x100, load = hw1 & 0x10;
    unsigned size = (hw1 >> 5) & 3, rn = hw1 & 0xf, rt = (hw2 >> 12) & 0xf;
    if (size == 3 || rn == 15 || rt == 15 || sign)
        return false;
    d->rn = rn, d->rd = rt;
    if (hw1 & 0x80) {
        d->imm = hw2 & 0xfff;
        static const dop_fn loads[3] = {h_ldrb_w, h_ldrh_w, h_ldr_w}, stores[3] = {h_strb_imm, h_strh_imm, h_str_imm};
        d->fn = load ? loads[size] : stores[size];
        return true;
    }
    if ((hw2 & 0x800) && size == 2) {
        uint32_t imm = hw2 & 0xff;
        d->imm = (hw2 & 0x200) ? imm : 0u - imm;
        d->lf |= ((hw2 & 0x400) ? LF_A : 0) | ((hw2 & 0x100) ? LF_B : 0);
        d->fn = load ? h_ldr_w8 : h_str_w8;
        return true;
    }
    return false;
}

// The same split as exec32, so each group's function is called directly, with the commonest
// forms decoded here instead.
static bool decode32(dop_t *d, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    d->fn = h_exec32;
    switch ((hw1 >> 11) & 3) {
    case 1:
        if ((hw1 & 0x0600) == 0x0000) {
            if (!(hw1 & 0x40))
                decode_ldm_stm(d, hw1, hw2);
            else if (!decode_ldrd_strd(d, hw1, hw2))
                d->fn = h_ldst_dual_excl;
        } else if ((hw1 & 0x0600) == 0x0200) {
            d->fn = h_dp_shifted_reg;
        }
        return false;
    case 2:
        if (hw2 & 0x8000) {
            unsigned op1 = (hw2 >> 12) & 7;
            uint32_t s = (hw1 >> 10) & 1, j1 = (hw2 >> 13) & 1, j2 = (hw2 >> 11) & 1;
            d->fn = h_branch_misc;
            if (op1 & 1) {
                uint32_t i1 = !(j1 ^ s), i2 = !(j2 ^ s);
                uint32_t imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((uint32_t)(hw1 & 0x3ff) << 12) |
                               ((uint32_t)(hw2 & 0x7ff) << 1);
                if (s)
                    imm |= 0xfe000000u;
                d->imm = pc + 4 + imm;
                d->fn = (op1 & 4) ? h_bl : h_branch;
            } else if (!(op1 & 4) && ((hw1 >> 7) & 7) != 7) {
                uint32_t imm = (s << 20) | (j2 << 19) | (j1 << 18) | ((uint32_t)(hw1 & 0x3f) << 12) |
                               ((uint32_t)(hw2 & 0x7ff) << 1);
                if (s)
                    imm |= 0xfff00000u;
                d->rd = (hw1 >> 6) & 0xf;
                d->imm = pc + 4 + imm;
                d->fn = h_bcond;
            }
            return true;
        }
        if (hw1 & 0x0200) {
            d->fn = h_dp_plain_imm;
        } else {
            uint32_t imm12 = ((uint32_t)(hw1 & 0x400) << 1) | ((hw2 >> 4) & 0x700) | (hw2 & 0xff), co;
            d->rm = (hw1 >> 5) & 0xf, d->rn = hw1 & 0xf, d->rd = (hw2 >> 8) & 0xf;
            d->imm = thumb_expand_imm_c(imm12, 0, &co);
            d->lf |= ((hw1 & 0x10) ? LF_A : 0) | ((imm12 >> 10) ? LF_B : 0);
            d->fn = h_dp_imm;
        }
        return false;
    case 3:
        if ((hw1 & 0x0e00) == 0x0800) {
            if (!decode_ldst_single(d, hw1, hw2))
                d->fn = h_ldst_single;
        } else if ((hw1 & 0x0f00) == 0x0a00)
            d->fn = h_dp_register;
        else if ((hw1 & 0x0f80) == 0x0b00)
            d->fn = h_multiply;
        else if ((hw1 & 0x0f80) == 0x0b80)
            d->fn = h_long_multiply;
        return false;
    default:
        return true;
    }
}

static inline bool is_it(uint16_t hw1)
{
    return (hw1 & 0xff00) == 0xbf00 && (hw1 & 0xf);
}

// Adds the instructions an IT instruction at b->ops[n - 1] covers, returning the new count, or 0
// if they do not all fit or one would end the block early.
static unsigned block_add_it(block_t *b, unsigned n, const uint8_t *page, uint32_t *ap, uint32_t *linep, bool xip)
{
    unsigned len = 4 - (unsigned)__builtin_ctz(b->ops[n - 1].hw1 & 0xf);
    uint32_t a = *ap, line = *linep;
    bool after_wide = false;
    if (n + len > BLK_MAX)
        return 0;
    for (unsigned i = 0; i < len; i++) {
        if ((a & 0xffff) == 0)
            return 0;
        uint16_t hw1 = rd16le(page + (a & 0xffff));
        bool wide = (hw1 >> 11) >= 0x1d;
        if ((wide && ((a + 2) & 0xffff) == 0) || is_it(hw1))
            return 0;
        dop_t *d = &b->ops[n++];
        d->pc = a;
        d->hw1 = hw1;
        d->hw2 = wide ? rd16le(page + ((a + 2) & 0xffff)) : 0;
        d->lf = wide ? 4 : 2;
        // A wide instruction whose condition fails never fetches its second halfword, so the next
        // one cannot know which line was fetched last.
        if (xip) {
            if ((a & ~7u) != line || after_wide)
                d->lf |= FETCH_HW1;
            line = a & ~7u;
            if (wide && ((a + 2) & ~7u) != line) {
                d->lf |= FETCH_HW2;
                line = (a + 2) & ~7u;
            }
        }
        after_wide = wide;
        bool last = wide ? decode32(d, a, hw1, d->hw2) : decode16(d, a, hw1);
        // The 16-bit forms depend on being in an IT block, which their handlers do not know.
        if (!wide)
            d->fn = h_exec16;
        if (last && i + 1 < len)
            return 0;
        a += d->lf & LF_LEN;
    }
    *ap = a;
    *linep = line;
    return n;
}

// Whether the ARMv6-M subset has this instruction as the blocks decode it: of the 32-bit ones
// only BL, MSR, MRS, the barriers and the hint space, and neither CBZ/CBNZ nor IT. Others are
// left to stepping, which treats them as the M0+ does.
static bool armv6m_block_op(uint16_t hw1, uint16_t hw2, bool wide)
{
    if (wide)
        return ((hw1 & 0xf800) == 0xf000 && (hw2 & 0xd000) == 0xd000) || (hw1 & 0xffe0) == 0xf3e0 ||
               (hw1 & 0xfff0) == 0xf380 || hw1 == 0xf3bf || hw1 == 0xf3af;
    return (hw1 & 0xf500) != 0xb100 && !((hw1 & 0xff00) == 0xbf00 && (hw1 & 0xf));
}

static bool block_build(chip_t *m, block_t *b, uint32_t pc, const uint8_t *page)
{
    bool xip = (pc >> 28) == 1;
    uint32_t a = pc, line = 1;
    unsigned n = 0, nrun = 0;
    while (n < BLK_MAX) {
        uint16_t hw1 = rd16le(page + (a & 0xffff));
        bool wide = (hw1 >> 11) >= 0x1d;
        // Both halves of an instruction must come from this page.
        if (wide && ((a + 2) & 0xffff) == 0)
            break;
        if (m->rp2040 && !armv6m_block_op(hw1, wide ? rd16le(page + ((a + 2) & 0xffff)) : 0, wide))
            break;
        dop_t *d = &b->ops[n++];
        d->pc = a;
        d->hw1 = hw1;
        d->hw2 = wide ? rd16le(page + ((a + 2) & 0xffff)) : 0;
        d->lf = wide ? 4 : 2;
        // Within a block the fetch line changes only as the block fetches (xip_flush ends the
        // block), so only the first fetch and fetches that cross into a new line need a look.
        if (xip) {
            if (n == 1 || (a & ~7u) != line)
                d->lf |= FETCH_HW1;
            line = a & ~7u;
            if (wide && ((a + 2) & ~7u) != line) {
                d->lf |= FETCH_HW2;
                line = (a + 2) & ~7u;
            }
        }
        bool last = wide ? decode32(d, a, hw1, d->hw2) : decode16(d, a, hw1);
        a += d->lf & LF_LEN;
        if (!wide && is_it(hw1)) {
            uint32_t a2 = a, line2 = line;
            unsigned n2 = block_add_it(b, n, page, &a2, &line2, xip);
            if (n2) {
                nrun = n;
                n = n2;
                a = a2;
            } else if (n > 1) {
                // The next block can start with the IT instruction and have room for them.
                n--;
                a -= 2;
            }
            break;
        }
        if (last || (a & 0xffff) == 0)
            break;
    }
    if (!n)
        return false;
    // A block may be run by either core, so it must be Secure code to both of their SAUs.
    if (!m->rp2040 && ((m->core[0].sau_ctrl | m->core[1].sau_ctrl) & 3))
        for (int k = 0; k < 2; k++) {
            uint32_t lo, hi;
            if (security_check(&m->core[k], pc, true, true, &lo, &hi).ns || a - 1 > hi) {
                // The ops were overwritten, so the entry must not match what it held before.
                b->tag = 0;
                return false;
            }
        }
    const uint8_t *src = page + (pc & 0xffff);
    bool fixed = (src >= m->rom && src < m->rom + ROM_SIZE) || (src >= m->flash && src < m->flash + m->flash_size);
    b->tag = pc | 1;
    b->gen = m->code_gen;
    b->fixed = fixed;
    b->jit = NULL;
    b->n = (uint16_t)n;
    b->nrun = (uint8_t)(nrun ? nrun : n);
    b->link_pc[0] = b->link_pc[1] = 1;
    b->bytes = (uint16_t)(a - pc);
    memcpy(b->code, src, b->bytes);
    return true;
}

static inline bool code_same(const uint8_t *a, const uint8_t *b, unsigned n)
{
    for (; n >= 8; n -= 8, a += 8, b += 8) {
        uint64_t x, y;
        memcpy(&x, a, 8);
        memcpy(&y, b, 8);
        if (x != y)
            return false;
    }
    for (; n; n -= 2, a += 2, b += 2)
        if (rd16le(a) != rd16le(b))
            return false;
    return true;
}

// Within one code generation an address always maps to the same ROM or flash bytes.
static inline bool block_valid(chip_t *m, const block_t *b, const uint8_t *page, uint32_t pc)
{
    return b->fixed ? b->gen == m->code_gen : code_same(page + (pc & 0xffff), b->code, b->bytes);
}

static inline bool block_still(chip_t *m, const block_t *b, uint32_t pc)
{
    if (b->tag != (pc | 1))
        return false;
    if (b->fixed)
        return b->gen == m->code_gen;
    const uint8_t *page = m->rpage[pc >> 16];
    return page && code_same(page + (pc & 0xffff), b->code, b->bytes);
}

static inline block_t *block_find(chip_t *m, uint32_t pc)
{
    const uint8_t *page = m->rpage[pc >> 16];
    if (unlikely(!page))
        return NULL;
    // Mixing in high address bits keeps SRAM and flash code with the same low bits apart.
    unsigned set = ((pc >> 1) ^ (pc >> 13) ^ (pc >> 25)) & (BLK_SETS - 1);
    block_t *w = blocks[set];
    for (unsigned i = 0; i < 2; i++)
        if (w[i].tag == (pc | 1) && block_valid(m, &w[i], page, pc)) {
            block_mru[set] = (uint8_t)i;
            return &w[i];
        }
    unsigned victim = w[0].tag == (pc | 1) ? 0 : w[1].tag == (pc | 1) ? 1 : !block_mru[set];
    if (!block_build(m, &w[victim], pc, page))
        return NULL;
    block_mru[set] = (uint8_t)victim;
    return &w[victim];
}

// A fault also needs the core's attention, so one load answers both.
static inline bool needs_look(const cpu_t *c)
{
    _Static_assert(offsetof(cpu_t, fault) == offsetof(cpu_t, check_exc) + 1, "flags must be adjacent");
    uint16_t v;
    memcpy(&v, &c->check_exc, sizeof(v));
    return v != 0;
}

// Runs blocks one after another until control reaches code that cannot be predecoded or the caller
// must look at the core: time is up, an exception may be due, or the core has stopped.
static void block_run(cpu_t *c, block_t *b, uint64_t until)
{
    chip_t *m = c->m;
    // A clock change sets check_exc, and so does the ROM when it advances c->t itself, so the
    // period and time only need reading again on the way out.
    const uint64_t ps = m->sys_ps;
    uint64_t t = c->t, insns = c->insns;
    const dop_t *d = b->ops, *end = b->ops + b->nrun;
    for (;;) {
        uint32_t pc = d->pc, next = pc + (d->lf & LF_LEN);
        uint32_t cyc = 1;
        if (d->lf & (FETCH_HW1 | FETCH_HW2)) {
            if ((d->lf & FETCH_HW1) && (pc & ~7u) != c->fetch_line) {
                cyc += xip_cost(m, pc);
                c->fetch_line = pc & ~7u;
            }
            if (d->lf & FETCH_HW2) {
                cyc += xip_cost(m, pc + 2);
                c->fetch_line = (pc + 2) & ~7u;
            }
        }
        c->cyc = cyc;
        c->r[15] = next;
        c->t = t;
        d->fn(c, d);
        insns++;
        if (unlikely(needs_look(c))) {
            if (c->fault)
                take_flagged_fault(c, pc, 0);
            t = c->t + (uint64_t)c->cyc * m->sys_ps;
            break;
        }
        t += (uint64_t)c->cyc * ps;
        if (t >= until)
            break;
        if (++d == end || c->r[15] != next) {
            // The core only stops or starts an IT block at an instruction that ends a block or
            // branches, and an IT block is then stepped one instruction at a time by cpu_run.
            if (c->itstate || c->state != CPU_RUN || c->ns || m->code_flush)
                break;
            uint32_t npc = c->r[15];
            unsigned k = npc == next ? 0 : 1;
            block_t *nb = b->link[k];
            if (b->link_pc[k] != npc || !block_still(m, nb, npc)) {
                if (!(nb = block_find(m, npc)))
                    break;
                b->link_pc[k] = npc;
                b->link[k] = nb;
            }
            b = nb;
            d = b->ops;
            end = b->ops + b->nrun;
        }
    }
    c->t = t;
    c->insns = insns;
}

static bool blocks_on(void)
{
    if (blocks_enabled < 0) {
        const char *e = getenv("PICOCALC_SIM_NO_BLOCKS");
        blocks_enabled = !(e && *e && *e != '0');
    }
    return blocks_enabled;
}
