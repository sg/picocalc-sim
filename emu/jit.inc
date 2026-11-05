// Native code for predecoded blocks on x86-64 hosts. Each block becomes straight-line machine code
// that does what block_run would do for it, so the emulation stays exactly the same as stepping.
//
// Most instructions are written out natively, with whatever can go differently (a load from a
// peripheral, a cache miss, a fault) left to the handler path, which calls the block handler as
// block_run does, with the cycle count, PC and time set up the same way. The guest registers and
// flags stay in cpu_t throughout, so a handler or an exit never has anything to write back. When a
// handler asks for a look, native code does what cpu_run would and, if nothing was taken, goes on.
// The instructions an IT instruction covers run here too, as step would run them.
//
// Time is checked once per block rather than once per instruction: on entry, a block whose fixed
// cycle costs could carry the time past the caller's limit goes instead to a careful translation
// of itself, made when first needed, which checks after every instruction as block_run does.
// Anything whose cost is only known as it runs (a cache miss, a slow memory access, a handler)
// checks again for the rest of the block once it has finished, and leaves for block_run if the
// rest might not fit.
//
// Native code checks its own block is still current when entered, as block_find would, since
// blocks jump straight to one another once linked.

#if defined(__x86_64__) && (defined(__linux__) || defined(_WIN32))
#define JIT_SUPPORTED 1
#endif

#ifdef JIT_SUPPORTED

#include <cpuid.h>

void *jit_alloc_exec(size_t size, const void *hint);

#ifndef JIT_SIZE
#define JIT_SIZE (64u << 20)
#endif
#define JIT_BLOCK_ROOM (32u << 10)
#define JIT_LOOKUP 32768
#define JIT_PSK 256

enum { JX_DONE, JX_CAREFUL, JX_LINK, JX_LOOKUP, JX_STALE, JX_CAREFUL0 };

// Everything native code reaches through r14.
typedef struct {
    uint64_t psk[JIT_PSK]; // k core clock periods in picoseconds
    uint64_t until;
    uint8_t *site; // what a link or stale exit wants patched
    // ARM flags from the byte LAHF leaves in AH; nzcc has the carry inverted, for subtraction.
    uint32_t nz[256], nzc[256], nzcc[256];
    struct {
        uint32_t pc, pad;
        void *code;
    } lookup[JIT_LOOKUP];
} jit_ctx_t;

// Kept just before each block's code so a translation can be found again from its address.
typedef struct {
    uint32_t pc, gen;
    uint16_t bytes;
    uint8_t fixed;
    uint8_t code[BLK_MAX * 4];
} jit_hdr_t;

static struct {
    int on;
    uint8_t *start, *end, *p;
    uint32_t epoch;
    uint32_t ps;
    int (*enter)(cpu_t *c, jit_ctx_t *x, void *code);
    uint8_t *exit_done, *exit_careful, *exit_link, *exit_lookup, *exit_stale, *exit_careful0, *exit_notime, *lookup;
} J = {.on = -1};

static jit_ctx_t jx;

// ---------------------------------------------------------------------------------------------
// x86-64 encoding

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum { CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G };
enum { ALU_ADD, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP };
enum { SH_SHL = 4, SH_SHR = 5, SH_SAR = 7 };

#ifdef _WIN32
#define ARG0 RCX
#define ARG1 RDX
#define ARG2 R8
#define ARG3 R9
#else
#define ARG0 RDI
#define ARG1 RSI
#define ARG2 RDX
#define ARG3 RCX
#endif

#define XM_RIP (-2)

typedef struct {
    int base, index, scale;
    int32_t disp; // the label for XM_RIP
    bool d32;     // always use a 32-bit displacement, so it can be patched
} xm_t;

static inline xm_t xm(int base, int32_t disp)
{
    return (xm_t){base, -1, 1, disp, false};
}

static inline xm_t xmi(int base, int index, int scale, int32_t disp)
{
    return (xm_t){base, index, scale, disp, false};
}

#define JIT_LABELS 1024
#define JIT_FIXUPS 2048

static uint8_t *lab_at[JIT_LABELS];
static int nlab;
static struct {
    uint8_t *site;
    int label, trailing;
} fixups[JIT_FIXUPS];
static int nfix;
static bool jit_overflow;

static int new_label(void)
{
    if (nlab == JIT_LABELS) {
        jit_overflow = true;
        return 0;
    }
    lab_at[nlab] = NULL;
    return nlab++;
}

static inline void bind(int l)
{
    lab_at[l] = J.p;
}

static void add_fixup(uint8_t *site, int label, int trailing)
{
    if (nfix == JIT_FIXUPS) {
        jit_overflow = true;
        return;
    }
    fixups[nfix].site = site;
    fixups[nfix].label = label;
    fixups[nfix].trailing = trailing;
    nfix++;
}

static inline void e8(unsigned v)
{
    *J.p++ = (uint8_t)v;
}

static inline void e32(uint32_t v)
{
    memcpy(J.p, &v, 4);
    J.p += 4;
}

static inline void e64(uint64_t v)
{
    memcpy(J.p, &v, 8);
    J.p += 8;
}

static inline int32_t rel_to(const void *target, const uint8_t *after)
{
    return (int32_t)((intptr_t)target - (intptr_t)after);
}

static inline bool in_rel32(const void *target, const uint8_t *after)
{
    intptr_t d = (intptr_t)target - (intptr_t)after;
    return d == (int32_t)d;
}

static void rex(int w, int reg, int index, int base, bool force)
{
    unsigned r = (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((index & 8) ? 2 : 0) | ((base & 8) ? 1 : 0);
    if (r || force)
        e8(0x40 | r);
}

static void opcode(unsigned opc)
{
    if (opc > 0xff)
        e8(opc >> 8);
    e8(opc);
}

static void modrm_mem(int reg, xm_t m, int trailing)
{
    reg &= 7;
    if (m.base == XM_RIP) {
        e8(0x05 | reg << 3);
        add_fixup(J.p, m.disp, trailing);
        e32(0);
        return;
    }
    int b = m.base & 7;
    int mod = m.d32 ? 2 : (m.disp == 0 && b != 5) ? 0 : (m.disp >= -128 && m.disp <= 127) ? 1 : 2;
    if (m.index >= 0 || b == 4) {
        int ss = m.scale == 8 ? 3 : m.scale == 4 ? 2 : m.scale == 2 ? 1 : 0;
        e8(mod << 6 | reg << 3 | 4);
        e8(ss << 6 | (m.index >= 0 ? (m.index & 7) : 4) << 3 | b);
    } else {
        e8(mod << 6 | reg << 3 | b);
    }
    if (mod == 1)
        e8((uint8_t)m.disp);
    else if (mod == 2)
        e32((uint32_t)m.disp);
}

static void op_m(int w, bool p66, unsigned opc, int reg, xm_t m, int trailing, bool force)
{
    if (p66)
        e8(0x66);
    rex(w, reg, m.index >= 0 ? m.index : 0, m.base == XM_RIP ? 0 : m.base, force);
    opcode(opc);
    modrm_mem(reg, m, trailing);
}

static void op_r(int w, bool p66, unsigned opc, int reg, int rm, bool force)
{
    if (p66)
        e8(0x66);
    rex(w, reg, 0, rm, force);
    opcode(opc);
    e8(0xc0 | (reg & 7) << 3 | (rm & 7));
}

static void mov_rm(int w, int r, xm_t m)
{
    op_m(w, false, 0x8b, r, m, 0, false);
}

static void mov_mr(int w, xm_t m, int r)
{
    op_m(w, false, 0x89, r, m, 0, false);
}

static void mov_rr(int w, int d, int s)
{
    op_r(w, false, 0x8b, d, s, false);
}

static void mov_mi(xm_t m, uint32_t imm)
{
    op_m(0, false, 0xc7, 0, m, 4, false);
    e32(imm);
}

static void mov_ri(int r, uint32_t imm)
{
    rex(0, 0, 0, r, false);
    e8(0xb8 + (r & 7));
    e32(imm);
}

static void mov_ri64(int r, uint64_t imm)
{
    rex(1, 0, 0, r, false);
    e8(0xb8 + (r & 7));
    e64(imm);
}

static void alu_rm(int w, int op, int r, xm_t m)
{
    op_m(w, false, op * 8 + 3, r, m, 0, false);
}

static void alu_mr(int w, int op, xm_t m, int r)
{
    op_m(w, false, op * 8 + 1, r, m, 0, false);
}

static void alu_rr(int w, int op, int d, int s)
{
    op_r(w, false, op * 8 + 3, d, s, false);
}

static void alu_ri(int w, int op, int r, int32_t imm)
{
    if (imm >= -128 && imm <= 127) {
        op_r(w, false, 0x83, op, r, false);
        e8((uint8_t)imm);
    } else {
        op_r(w, false, 0x81, op, r, false);
        e32((uint32_t)imm);
    }
}

static void alu_mi(int w, int op, xm_t m, int32_t imm)
{
    if (imm >= -128 && imm <= 127) {
        op_m(w, false, 0x83, op, m, 1, false);
        e8((uint8_t)imm);
    } else {
        op_m(w, false, 0x81, op, m, 4, false);
        e32((uint32_t)imm);
    }
}

static void cmp16_mi(xm_t m, int8_t imm)
{
    op_m(0, true, 0x83, 7, m, 1, false);
    e8((uint8_t)imm);
}

static void test_rr(int a, int b)
{
    op_r(0, false, 0x85, b, a, false);
}

// TEST with an immediate, as a byte test where the register and value allow.
static void test_ri(int r, uint32_t imm)
{
    if (imm < 0x100 && r < 4) {
        op_r(0, false, 0xf6, 0, r, false);
        e8(imm);
    } else {
        op_r(0, false, 0xf7, 0, r, false);
        e32(imm);
    }
}

static void test_mi8(xm_t m, uint8_t imm)
{
    op_m(0, false, 0xf6, 0, m, 1, false);
    e8(imm);
}

static void shift_ri(int kind, int r, int n)
{
    if (n == 1) {
        op_r(0, false, 0xd1, kind, r, false);
    } else {
        op_r(0, false, 0xc1, kind, r, false);
        e8(n);
    }
}

static void imul_rm(int w, int r, xm_t m)
{
    op_m(w, false, 0x0faf, r, m, 0, false);
}

static void lea(int w, int r, xm_t m)
{
    op_m(w, false, 0x8d, r, m, 0, false);
}

static void lea_rip_abs(int r, const void *target)
{
    rex(1, r, 0, 0, false);
    e8(0x8d);
    e8(0x05 | (r & 7) << 3);
    e32(rel_to(target, J.p + 4));
}

static void movzx_ah(int d)
{
    op_r(0, false, 0x0fb6, d, 4, false);
}

static void setcc(int cc, int r)
{
    op_r(0, false, 0x0f90 + cc, 0, r, r >= 4);
}

static void bt_mi(xm_t m, int bit)
{
    op_m(0, false, 0x0fba, 4, m, 1, false);
    e8(bit);
}

static void not_r(int r)
{
    op_r(0, false, 0xf7, 2, r, false);
}

static void push_r(int r)
{
    rex(0, 0, 0, r, false);
    e8(0x50 + (r & 7));
}

static void pop_r(int r)
{
    rex(0, 0, 0, r, false);
    e8(0x58 + (r & 7));
}

static void jcc_l(int cc, int l)
{
    e8(0x0f);
    e8(0x80 + cc);
    add_fixup(J.p, l, 0);
    e32(0);
}

static void jmp_l(int l)
{
    e8(0xe9);
    add_fixup(J.p, l, 0);
    e32(0);
}

static void jcc_a(int cc, const void *t)
{
    e8(0x0f);
    e8(0x80 + cc);
    e32(rel_to(t, J.p + 4));
}

static void jmp_a(const void *t)
{
    e8(0xe9);
    e32(rel_to(t, J.p + 4));
}

static void call_a(const void *t)
{
    if (in_rel32(t, J.p + 5)) {
        e8(0xe8);
        e32(rel_to(t, J.p + 4));
    } else {
        mov_ri64(RAX, (uint64_t)(uintptr_t)t);
        op_r(0, false, 0xff, 2, RAX, false);
    }
}

// ---------------------------------------------------------------------------------------------
// Layout of the state native code works on

#define CO(f) ((int32_t)offsetof(cpu_t, f))
#define MO(f) ((int32_t)offsetof(chip_t, f))
#define XO(f) ((int32_t)offsetof(jit_ctx_t, f))

static inline xm_t RG(unsigned n)
{
    return xm(RBX, CO(r) + 4 * (int32_t)n);
}

#define APSR xm(RBX, CO(apsr))
#define PSK(k) xm(R14, XO(psk) + 8 * (int32_t)(k))

// rbx: the core; r12: the time, less the fixed cycles of the instructions passed since it was last
// brought up to date; r13: the limit; r14: jx; r15: the chip.

// ---------------------------------------------------------------------------------------------
// Fixed code: the way in and out, and the lookup of a block from its address

static void jit_emit_fixed(void)
{
    J.p = J.start;
    J.enter = (int (*)(cpu_t *, jit_ctx_t *, void *))(void *)J.p;
    push_r(RBX);
    push_r(RBP);
#ifdef _WIN32
    push_r(RDI);
    push_r(RSI);
#endif
    push_r(R12);
    push_r(R13);
    push_r(R14);
    push_r(R15);
#ifdef _WIN32
    alu_ri(1, ALU_SUB, RSP, 40); // the callee's shadow space, keeping calls 16-byte aligned
    mov_rr(1, RBX, RCX);
    mov_rr(1, R14, RDX);
#define JIT_CODE_ARG R8
#else
    alu_ri(1, ALU_SUB, RSP, 8);
    mov_rr(1, RBX, RDI);
    mov_rr(1, R14, RSI);
#define JIT_CODE_ARG RDX
#endif
    mov_rm(1, R15, xm(RBX, CO(m)));
    mov_rm(1, R12, xm(RBX, CO(t)));
    mov_rm(1, R13, xm(R14, XO(until)));
    op_r(0, false, 0xff, 4, JIT_CODE_ARG, false);

    uint8_t *epi_time = J.p;
    mov_mr(1, xm(RBX, CO(t)), R12);
    uint8_t *epi = J.p;
#ifdef _WIN32
    alu_ri(1, ALU_ADD, RSP, 40);
#else
    alu_ri(1, ALU_ADD, RSP, 8);
#endif
    pop_r(R15);
    pop_r(R14);
    pop_r(R13);
    pop_r(R12);
#ifdef _WIN32
    pop_r(RSI);
    pop_r(RDI);
#endif
    pop_r(RBP);
    pop_r(RBX);
    e8(0xc3);

    static const int codes[] = {JX_DONE, JX_CAREFUL, JX_LINK, JX_LOOKUP, JX_STALE, JX_CAREFUL0};
    uint8_t **exits[] = {&J.exit_done,   &J.exit_careful, &J.exit_link,
                         &J.exit_lookup, &J.exit_stale,   &J.exit_careful0};
    for (int i = 0; i < 6; i++) {
        *exits[i] = J.p;
        mov_ri(RAX, (uint32_t)codes[i]);
        jmp_a(epi_time);
    }
    J.exit_notime = J.p;
    mov_ri(RAX, JX_DONE);
    jmp_a(epi);

    // eax: the address to go to; the hash must match jit_hash.
    J.lookup = J.p;
    mov_rr(0, RCX, RAX);
    shift_ri(SH_SHR, RCX, 17);
    mov_rr(0, RDX, RAX);
    shift_ri(SH_SHR, RDX, 1);
    alu_rr(0, ALU_XOR, RCX, RDX);
    alu_ri(0, ALU_AND, RCX, JIT_LOOKUP - 1);
    shift_ri(SH_SHL, RCX, 4);
    alu_rm(0, ALU_CMP, RAX, xmi(R14, RCX, 1, XO(lookup)));
    e8(0x75); // jne +n to the miss
    uint8_t *jne = J.p;
    e8(0);
    op_m(0, false, 0xff, 4, xmi(R14, RCX, 1, XO(lookup) + 8), 0, false);
    *jne = (uint8_t)(J.p - jne - 1);
    mov_mr(0, RG(15), RAX);
    jmp_a(J.exit_lookup);

    J.p = (uint8_t *)(((uintptr_t)J.p + 63) & ~(uintptr_t)63);
}

static inline unsigned jit_hash(uint32_t pc)
{
    return ((pc >> 17) ^ (pc >> 1)) & (JIT_LOOKUP - 1);
}

static uint8_t *jit_code_start;

static void jit_flush(void)
{
    J.p = jit_code_start;
    J.epoch++;
    memset(jx.lookup, 0, sizeof(jx.lookup));
}

static bool jit_init(void)
{
    const char *e = getenv("PICOCALC_SIM_NO_JIT");
    if (e && *e && *e != '0')
        return false;
    // The flags are read with LAHF, which a few of the earliest x86-64 processors lack.
    unsigned a, b, c, d;
    if (!__get_cpuid(0x80000001u, &a, &b, &c, &d) || !(c & 1))
        return false;
    uint8_t *mem = jit_alloc_exec(JIT_SIZE, (const void *)&cpu_run);
    if (!mem)
        return false;
    J.start = mem;
    J.end = mem + JIT_SIZE;
    jit_emit_fixed();
    jit_code_start = J.p;
    for (unsigned ah = 0; ah < 256; ah++) {
        uint32_t nz = ((ah & 0x80) ? F_N : 0) | ((ah & 0x40) ? F_Z : 0);
        jx.nz[ah] = nz;
        jx.nzc[ah] = nz | ((ah & 1) ? F_C : 0);
        jx.nzcc[ah] = nz | ((ah & 1) ? 0 : F_C);
    }
    J.epoch = 1;
    return true;
}

static bool jit_available(void)
{
    if (J.on < 0)
        J.on = jit_init();
    return J.on;
}

static void jit_forget(void)
{
    if (J.on > 0)
        jit_flush();
}

static void jit_set_ps(uint32_t ps);

// What block_run does when a handler asks for a look: a fault is taken (it is the IT state of the
// instruction), and the time catches up with the handler's cycles at whatever clock it left
// behind. Then what cpu_run would do before running a block again, returning whether native code
// can go straight on at c->r[15].
static bool jit_look(cpu_t *c, uint32_t pc, uint32_t it)
{
    chip_t *m = c->m;
    if (c->fault) {
        unsigned exc = c->fault_usage ? EXC_USAGEFAULT : EXC_BUSFAULT;
        c->r[15] = pc;
        c->itstate = it;
        take_fault(c, exc, pc, exc == EXC_USAGEFAULT ? "unaligned access" : "bus fault");
    }
    c->t += (uint64_t)c->cyc * m->sys_ps;
    if (c->t >= jx.until)
        return false;
    if (c->check_exc) {
        check_exceptions(c);
        if (c->state != CPU_RUN)
            sleep_check(c);
    }
    // An exception just taken leaves check_exc set, which block_run answers after one instruction.
    if (c->state != CPU_RUN || c->itstate || c->check_exc)
        return false;
    if (m->sys_ps != J.ps)
        jit_set_ps(m->sys_ps);
    return true;
}

// An instruction an IT instruction covers, as step runs it once its first halfword is fetched.
static void jit_it_op(cpu_t *c, const dop_t *d)
{
    uint32_t it = c->itstate;
    c->r[15] = d->pc + (d->lf & LF_LEN);
    if (!cond_pass(c, it >> 4)) {
        it_advance(c);
        return;
    }
    if (d->lf & FETCH_HW2) {
        c->cyc += xip_cost(c->m, d->pc + 2);
        c->fetch_line = (d->pc + 2) & ~7u;
    }
    d->fn(c, d);
    if (!c->fault && c->itstate == it)
        it_advance(c);
}

// ---------------------------------------------------------------------------------------------
// Compiling a block

static struct {
    const block_t *b;
    chip_t *m;
    int n;
    bool fixed;
    bool careful; // checks the time after every instruction
    bool ended;   // the last instruction has sent control on
    uint8_t *pre_site; // the entry's time check, patched to go to the careful translation
    int sb[BLK_MAX + 1]; // fixed cycles before each instruction
    int total;           // fixed cycles of the whole block on its dearest path
    bool generic[BLK_MAX];
    int l_main[BLK_MAX + 1], l_gen1[BLK_MAX], l_gencyc[BLK_MAX], l_fetch1[BLK_MAX], l_fetch2[BLK_MAX];
    int l_careful[BLK_MAX + 1], l_look[BLK_MAX], l_dyn[BLK_MAX], l_dop[BLK_MAX], l_mmio[BLK_MAX];
    // A single load or store that can call the bus itself for a peripheral address.
    struct {
        uint8_t size, reg;
        bool load, sign, keep_on_fault;
    } mm[BLK_MAX];
    int l_fall, l_stale;
    bool fall_bound;
    uint8_t it[BLK_MAX]; // the IT state each instruction an IT instruction covers starts with
    bool nf;             // emitting a 16-bit instruction under IT, which sets no flags
    int l_skip[BLK_MAX]; // where an instruction under IT goes when its condition fails
    int l_post[BLK_MAX]; // just after an instruction, before a careful translation's time check
    // Where a load or store whose page is not plain memory goes, and where a load's value, in
    // eax, carries on from.
    int l_other[BLK_MAX], l_loaded[BLK_MAX];
    struct {
        uint8_t size;
        bool sign;
    } ld[BLK_MAX];
    // Fixed-address XIP accesses that missed their set's last line, to look in both ways for.
    struct {
        int label, hit, miss;
        uint32_t set, tag;
    } xipw[4 * BLK_MAX];
    int nxipw;
    uint8_t *total_sites[8 * BLK_MAX];
    int ntotal;
    struct {
        uint8_t *site;
        uint32_t target;
    } links[2];
    int nlinks;
} K;

static inline int lab(int *slot)
{
    if (*slot < 0)
        *slot = new_label();
    return *slot;
}

static inline uint32_t op_next(const dop_t *d)
{
    return d->pc + (d->lf & LF_LEN);
}

// Normally r12 + psk[total] < until, or leave at instruction k, for the careful translation at the
// entry and for block_run elsewhere. In a careful translation, stop at instruction k once the time
// has reached the limit.
static void emit_budget_check(int k)
{
    mov_rr(1, RAX, R12);
    if (K.careful) {
        if (K.sb[k])
            alu_rm(1, ALU_ADD, RAX, PSK(K.sb[k]));
    } else {
        xm_t m = PSK(0);
        m.d32 = true;
        op_m(1, false, 0x03, RAX, m, 0, false);
        if (K.ntotal < (int)(sizeof(K.total_sites) / sizeof(K.total_sites[0])))
            K.total_sites[K.ntotal++] = J.p - 4;
        else
            jit_overflow = true;
    }
    alu_rr(1, ALU_CMP, RAX, R13);
    jcc_l(CC_AE, lab(&K.l_careful[k]));
}

// In a careful translation, with r12 brought up to date: stop if the time has reached the limit,
// with the PC from target, or from eax when target is 1.
static void emit_end_check(uint32_t target)
{
    if (!K.careful)
        return;
    alu_rr(1, ALU_CMP, R12, R13);
    e8(0x72); // jb over the stop
    uint8_t *jb = J.p;
    e8(0);
    if (target == 1)
        mov_mr(0, RG(15), RAX);
    else
        mov_mi(RG(15), target);
    jmp_a(J.exit_done);
    *jb = (uint8_t)(J.p - jb - 1);
}

// The time at instruction k, into c->t.
static void emit_store_time(int k)
{
    if (K.sb[k]) {
        mov_rr(1, RAX, R12);
        alu_rm(1, ALU_ADD, RAX, PSK(K.sb[k]));
        mov_mr(1, xm(RBX, CO(t)), RAX);
    } else {
        mov_mr(1, xm(RBX, CO(t)), R12);
    }
}

static void emit_add_insns(int n)
{
    if (n)
        alu_mi(1, ALU_ADD, xm(RBX, CO(insns)), n);
}

// A jump to another block, patched to go there directly once the block has been found.
static void emit_link(uint32_t target)
{
    emit_end_check(target);
    e8(0xe9);
    K.links[K.nlinks].site = J.p;
    K.links[K.nlinks].target = target;
    K.nlinks++;
    e32(0);
}

// xip_access for a hit, which costs nothing: the line its set used last here, either way out of
// line; anything else goes to miss.
static void emit_xip_hit(uint32_t addr, int miss)
{
    if ((addr >> 24) >= 0x14 || K.nxipw == (int)(sizeof(K.xipw) / sizeof(K.xipw[0]))) {
        jmp_l(miss);
        return;
    }
    uint32_t line = (addr & 0x01ffffffu) >> 3, set = line & 1023, tag = (line >> 10) | 0x80000000u;
    int hit = new_label(), way = new_label();
    test_mi8(xm(R15, MO(xip_ctrl)), 1);
    jcc_l(CC_E, miss);
    alu_mi(0, ALU_CMP, xm(R15, MO(xip_mru) + 4 * (int32_t)set), (int32_t)tag);
    jcc_l(CC_NE, way);
    bind(hit);
    alu_mi(0, ALU_ADD, xm(R15, MO(xip_ctr_acc)), 1);
    alu_mi(0, ALU_ADD, xm(R15, MO(xip_ctr_hit)), 1);
    K.xipw[K.nxipw].label = way;
    K.xipw[K.nxipw].hit = hit;
    K.xipw[K.nxipw].miss = miss;
    K.xipw[K.nxipw].set = set;
    K.xipw[K.nxipw].tag = tag;
    K.nxipw++;
}

// The rest of a hit for emit_xip_hit: the way found becomes the set's most recent.
static void emit_xip_ways(void)
{
    for (int i = 0; i < K.nxipw; i++) {
        int32_t set = (int32_t)K.xipw[i].set, tag = (int32_t)K.xipw[i].tag;
        int way1 = new_label(), done = new_label();
        bind(K.xipw[i].label);
        alu_mi(0, ALU_CMP, xm(R15, MO(xip_tag) + 8 * set), tag);
        jcc_l(CC_NE, way1);
        op_m(0, false, 0xc6, 0, xm(R15, MO(xip_lru) + set), 1, false);
        e8(1);
        jmp_l(done);
        bind(way1);
        alu_mi(0, ALU_CMP, xm(R15, MO(xip_tag) + 8 * set + 4), tag);
        jcc_l(CC_NE, K.xipw[i].miss);
        op_m(0, false, 0xc6, 0, xm(R15, MO(xip_lru) + set), 1, false);
        e8(0);
        bind(done);
        mov_mi(xm(R15, MO(xip_mru) + 4 * set), (uint32_t)tag);
        jmp_l(K.xipw[i].hit);
    }
}

static void emit_fetch(int k, const dop_t *d)
{
    uint32_t pc = d->pc;
    if (d->lf & FETCH_HW1) {
        int same = -1;
        // Later instructions in a block always fetch from a new line when flagged, unless a wide
        // one before them in an IT block might not have fetched its second halfword.
        if (k == 0 || (K.it[k] && (K.b->ops[k - 1].lf & LF_LEN) == 4)) {
            alu_mi(0, ALU_CMP, xm(RBX, CO(fetch_line)), (int32_t)(pc & ~7u));
            same = new_label();
            jcc_l(CC_E, same);
        }
        emit_xip_hit(pc, lab(&K.l_fetch1[k]));
        mov_mi(xm(RBX, CO(fetch_line)), pc & ~7u);
        if (same >= 0)
            bind(same);
    }
    // Under IT, the second halfword is fetched only once the condition passes.
    if ((d->lf & FETCH_HW2) && !K.it[k]) {
        emit_xip_hit(pc + 2, lab(&K.l_fetch2[k]));
        mov_mi(xm(RBX, CO(fetch_line)), (pc + 2) & ~7u);
    }
}

// The handler call, as block_run makes it, with c->cyc already set. st is what the native version
// of the instruction counts as fixed, which r12 must leave out afterwards.
static void emit_generic_body(int k, const dop_t *d, int st, bool cold)
{
    uint32_t next = op_next(d);
    mov_mi(RG(15), next);
    emit_store_time(k);
    mov_rr(1, ARG0, RBX);
    op_m(1, false, 0x8d, ARG1, (xm_t){XM_RIP, -1, 1, lab(&K.l_dop[k]), false}, 0, false);
    call_a(K.it[k] ? (const void *)jit_it_op : (const void *)d->fn);
    cmp16_mi(xm(RBX, CO(check_exc)), 0);
    jcc_l(CC_NE, lab(&K.l_look[k]));
    mov_rm(0, RAX, xm(RBX, CO(cyc)));
    imul_rm(1, RAX, PSK(1));
    alu_rr(1, ALU_ADD, R12, RAX);
    if (st)
        alu_rm(1, ALU_SUB, R12, PSK(st));
    alu_mi(0, ALU_CMP, RG(15), (int32_t)next);
    jcc_l(CC_NE, lab(&K.l_dyn[k]));
    emit_budget_check(k + 1);
    if (cold)
        jmp_l(k + 1 < K.n ? K.l_main[k + 1] : K.l_fall);
}

// The address of a load or store in eax, its page in rdx and offset in eax, or off to K.l_other
// when the page is not plain memory.
static void emit_page(int k, int32_t table, unsigned align_mask, uint32_t span)
{
    mov_rr(0, RCX, RAX);
    shift_ri(SH_SHR, RCX, 16);
    mov_rm(1, RDX, xmi(R15, RCX, 8, table));
    test_rr(RDX, RDX);
    jcc_l(CC_E, lab(&K.l_other[k]));
    if (align_mask) {
        test_ri(RAX, align_mask);
        jcc_l(CC_NE, lab(&K.l_gen1[k]));
    }
    op_r(0, false, 0x0fb7, RAX, RAX, false); // movzx eax, ax
    if (span) {
        alu_ri(0, ALU_CMP, RAX, (int32_t)(0x10000 - span));
        jcc_l(CC_A, lab(&K.l_gen1[k]));
    }
}

static void emit_host_load(int size, bool sign, xm_t at)
{
    if (size == 4)
        mov_rm(0, RAX, at);
    else if (size == 2)
        op_m(0, false, sign ? 0x0fbf : 0x0fb7, RAX, at, 0, false);
    else
        op_m(0, false, sign ? 0x0fbe : 0x0fb6, RAX, at, 0, false);
}

// Loads into eax; size 1 or 2 may sign-extend.
static void emit_load_value(int k, int size, bool sign)
{
    K.ld[k].size = (uint8_t)size;
    K.ld[k].sign = sign;
    emit_page(k, MO(rpage_plain), size == 4 ? 3 : size == 2 ? 1 : 0, 0);
    emit_host_load(size, sign, xmi(RDX, RAX, 1, 0));
    bind(lab(&K.l_loaded[k]));
}

static void emit_store_value(int k, int size, unsigned rt)
{
    emit_page(k, MO(wpage), size == 4 ? 3 : size == 2 ? 1 : 0, 0);
    mov_rm(0, RCX, RG(rt));
    xm_t at = xmi(RDX, RAX, 1, 0);
    if (size == 4)
        mov_mr(0, at, RCX);
    else if (size == 2)
        op_m(0, true, 0x89, RCX, at, 0, false);
    else
        op_m(0, false, 0x88, RCX, at, 0, false);
}

static void emit_addr_imm(unsigned rn, uint32_t imm)
{
    mov_rm(0, RAX, RG(rn));
    if (imm)
        alu_ri(0, ALU_ADD, RAX, (int32_t)imm);
}

// APSR = (APSR & keep) | eax.
static void emit_merge_flags(uint32_t keep)
{
    mov_rm(0, RCX, APSR);
    alu_ri(0, ALU_AND, RCX, (int32_t)keep);
    alu_rr(0, ALU_OR, RCX, RAX);
    mov_mr(0, APSR, RCX);
}

// N and Z from SF and ZF as they stand; C becomes carry when it is 0 or 1, and stays otherwise.
static void emit_flags_nz(int carry)
{
    e8(0x9f); // lahf
    movzx_ah(RAX);
    mov_rm(0, RAX, xmi(R14, RAX, 4, XO(nz)));
    if (carry == 1)
        alu_ri(0, ALU_OR, RAX, (int32_t)F_C);
    emit_merge_flags(carry < 0 ? ~(F_N | F_Z) : ~(F_N | F_Z | F_C));
}

// N, Z and C from an x86 shift that matches the ARM one.
static void emit_flags_nzc(void)
{
    e8(0x9f);
    movzx_ah(RAX);
    mov_rm(0, RAX, xmi(R14, RAX, 4, XO(nzc)));
    emit_merge_flags(~(F_N | F_Z | F_C));
}

// All four after an x86 add or subtract, with edx zeroed before it; subtraction inverts carry.
static void emit_flags_arith(bool sub)
{
    setcc(CC_O, RDX);
    e8(0x9f);
    movzx_ah(RAX);
    mov_rm(0, RAX, xmi(R14, RAX, 4, sub ? XO(nzcc) : XO(nzc)));
    shift_ri(SH_SHL, RDX, 28);
    alu_rr(0, ALU_OR, RAX, RDX);
    emit_merge_flags(0x0fffffffu);
}

// N and Z of a value known now; carry as for emit_flags_nz.
static void emit_flags_const(uint32_t v, int carry)
{
    uint32_t mask = F_N | F_Z | (carry >= 0 ? F_C : 0);
    uint32_t set = (v & F_N) | (v ? 0 : F_Z) | (carry == 1 ? F_C : 0);
    alu_mi(0, ALU_AND, APSR, (int32_t)~mask);
    if (set)
        alu_mi(0, ALU_OR, APSR, (int32_t)set);
}

// Taken if the condition holds for the flags in eax.
static void emit_cond_jump(unsigned cond, int taken)
{
    switch (cond) {
    case 0x0: test_ri(RAX, F_Z); jcc_l(CC_NE, taken); break;
    case 0x1: test_ri(RAX, F_Z); jcc_l(CC_E, taken); break;
    case 0x2: test_ri(RAX, F_C); jcc_l(CC_NE, taken); break;
    case 0x3: test_ri(RAX, F_C); jcc_l(CC_E, taken); break;
    case 0x4: test_rr(RAX, RAX); jcc_l(CC_S, taken); break;
    case 0x5: test_rr(RAX, RAX); jcc_l(CC_NS, taken); break;
    case 0x6: test_ri(RAX, F_V); jcc_l(CC_NE, taken); break;
    case 0x7: test_ri(RAX, F_V); jcc_l(CC_E, taken); break;
    case 0x8:
    case 0x9:
        alu_ri(0, ALU_AND, RAX, (int32_t)(F_C | F_Z));
        alu_ri(0, ALU_CMP, RAX, (int32_t)F_C);
        jcc_l(cond == 0x8 ? CC_E : CC_NE, taken);
        break;
    case 0xa:
    case 0xb:
        // N ^ V into bit 31.
        mov_rr(0, RCX, RAX);
        shift_ri(SH_SHL, RCX, 3);
        alu_rr(0, ALU_XOR, RCX, RAX);
        jcc_l(cond == 0xa ? CC_NS : CC_S, taken);
        break;
    default:
        // (N ^ V) | Z into bit 31.
        mov_rr(0, RCX, RAX);
        shift_ri(SH_SHL, RCX, 3);
        alu_rr(0, ALU_XOR, RCX, RAX);
        lea(0, RDX, xmi(RAX, RAX, 1, 0));
        alu_rr(0, ALU_OR, RCX, RDX);
        jcc_l(cond == 0xc ? CC_NS : CC_S, taken);
        break;
    }
}

// The end of a block that may go two ways: fall through with fixed cost st, or be taken with one
// cycle more.
static void emit_two_way(int k, int st, int taken, uint32_t target)
{
    K.l_fall = K.l_fall < 0 ? new_label() : K.l_fall;
    bind(K.l_fall);
    K.fall_bound = true;
    alu_rm(1, ALU_ADD, R12, PSK(K.sb[k] + st));
    emit_add_insns(K.n);
    emit_link(op_next(&K.b->ops[k]));
    bind(taken);
    alu_rm(1, ALU_ADD, R12, PSK(K.sb[k] + st + 1));
    emit_add_insns(K.n);
    emit_link(target);
    K.ended = true;
}

// The end of a block that goes on to an address in eax.
static void emit_to_eax(int k, int st)
{
    alu_rm(1, ALU_ADD, R12, PSK(K.sb[k] + st));
    emit_add_insns(k + 1);
    emit_end_check(1);
    jmp_a(J.lookup);
    K.ended = true;
}

// An interworking branch target in eax that block_run's handler would treat specially (an
// exception return, or a clear Thumb bit) goes to the handler; otherwise the bit is cleared.
static void emit_check_target(int k)
{
    alu_ri(0, ALU_CMP, RAX, (int32_t)0xff000000u);
    jcc_l(CC_AE, lab(&K.l_gen1[k]));
    test_ri(RAX, 1);
    jcc_l(CC_E, lab(&K.l_gen1[k]));
    alu_ri(0, ALU_AND, RAX, -2);
}

// After an instruction of fixed cost st that has asked for a look: what block_run does then.
static void emit_inline_look(int k, int st)
{
    mov_mi(xm(RBX, CO(cyc)), (uint32_t)st);
    mov_mi(RG(15), op_next(&K.b->ops[k]));
    emit_store_time(k);
    jmp_l(lab(&K.l_look[k]));
    K.ended = true;
}

// MRS and MSR of the masks, CPS, barriers and the hints that do nothing.
static int emit_system(int k, const dop_t *d)
{
    if (d->fn == h_exec16 && (d->hw1 & 0xffec) == 0xb660) {
        uint32_t disable = (d->hw1 >> 4) & 1;
        if (d->hw1 & 2)
            mov_mi(xm(RBX, CO(primask)), disable);
        if (d->hw1 & 1)
            mov_mi(xm(RBX, CO(faultmask)), disable);
        op_m(0, false, 0xc6, 0, xm(RBX, CO(check_exc)), 1, false);
        e8(1);
        emit_inline_look(k, 1);
        return 1;
    }
    if (d->fn != h_branch_misc || ((d->hw1 >> 7) & 7) != 7 || (d->hw2 & 0x5000))
        return -1;
    unsigned op = (d->hw1 >> 4) & 0x7f, sysm = d->hw2 & 0xff;
    if (op == 0x3e || op == 0x3f) {
        unsigned rd = (d->hw2 >> 8) & 0xf;
        if (rd == 15)
            return -1;
        if (sysm <= 2 && sysm != 1) {
            mov_rm(0, RAX, APSR);
            alu_ri(0, ALU_AND, RAX, (int32_t)0xf80f0000u);
        } else if (sysm == 16 || sysm == 19) {
            mov_rm(0, RAX, xm(RBX, sysm == 16 ? CO(primask) : CO(faultmask)));
            alu_ri(0, ALU_AND, RAX, 1);
        } else if (sysm == 17 || sysm == 18) {
            mov_rm(0, RAX, xm(RBX, CO(basepri)));
        } else {
            return -1;
        }
        mov_mr(0, RG(rd), RAX);
        return 1;
    }
    if ((op == 0x38 || op == 0x39) && sysm == 16 && (d->hw1 & 0xf) != 15) {
        mov_rm(0, RAX, RG(d->hw1 & 0xf));
        alu_ri(0, ALU_AND, RAX, 1);
        mov_mr(0, xm(RBX, CO(primask)), RAX);
        op_m(0, false, 0xc6, 0, xm(RBX, CO(check_exc)), 1, false);
        e8(1);
        emit_inline_look(k, 1);
        return 1;
    }
    if (op == 0x3a && (sysm < 2 || sysm > 4))
        return 1;
    if (op == 0x3b) {
        if (((d->hw2 >> 4) & 0xf) == 2) {
            op_m(0, false, 0xc6, 0, xm(RBX, CO(excl_valid)), 1, false);
            e8(0);
        }
        return 2;
    }
    return -1;
}

// The loads and stores ldst_single handles that decode_ldst_single left to it: sign-extending
// loads, register offsets, 8-bit offsets with writeback, and loads into the PC.
static int emit_ldst_single(int k, const dop_t *d)
{
    uint16_t hw1 = d->hw1, hw2 = d->hw2;
    bool sign = hw1 & 0x100, load = hw1 & 0x10;
    unsigned size = (hw1 >> 5) & 3, rn = hw1 & 0xf, rt = (hw2 >> 12) & 0xf;
    if (size == 3 || rn == 15 || (!load && (sign || rt == 15)))
        return -1;
    bool wb = false, index = true;
    int rm = -1;
    unsigned shift = 0;
    uint32_t imm = 0;
    if (hw1 & 0x80) {
        imm = hw2 & 0xfff;
    } else if (hw2 & 0x800) {
        index = hw2 & 0x400;
        wb = hw2 & 0x100;
        imm = (hw2 & 0x200) ? (hw2 & 0xffu) : 0u - (hw2 & 0xffu);
    } else if ((hw2 & 0xfc0) == 0) {
        rm = hw2 & 0xf;
        shift = (hw2 >> 4) & 3;
        if (rm == 15)
            return -1;
    } else {
        return -1;
    }
    // Preloads, which do nothing here.
    if (load && rt == 15 && size != 2)
        return 1;
    if (wb && rn == rt)
        return -1;
    int bytes = 1 << size;
    if (!wb && rt != 15) {
        K.mm[k].size = (uint8_t)bytes;
        K.mm[k].reg = (uint8_t)rt;
        K.mm[k].load = load;
        K.mm[k].sign = sign;
        K.mm[k].keep_on_fault = true;
    }
    mov_rm(0, RAX, RG(rn));
    if (rm >= 0) {
        mov_rm(0, RCX, RG(rm));
        if (shift)
            shift_ri(SH_SHL, RCX, (int)shift);
        alu_rr(0, ALU_ADD, RAX, RCX);
    } else if (index && imm) {
        alu_ri(0, ALU_ADD, RAX, (int32_t)imm);
    }
    if (load)
        emit_load_value(k, bytes, sign);
    else
        emit_store_value(k, bytes, rt);
    if (load && rt == 15)
        emit_check_target(k);
    if (wb) {
        mov_rm(0, RCX, RG(rn));
        alu_ri(0, ALU_ADD, RCX, (int32_t)imm);
        mov_mr(0, RG(rn), RCX);
    }
    if (load && rt == 15) {
        emit_to_eax(k, 4);
        return 4;
    }
    if (load)
        mov_mr(0, RG(rt), RAX);
    return 2;
}

// LDM and STM, with PUSH.W and POP.W among them.
static int emit_ldm_stm(int k, const dop_t *d)
{
    unsigned rn = d->rn, n = d->rm;
    uint32_t list = d->imm;
    bool load = d->lf & LF_A, wb = d->lf & LF_B, db = d->lf & LF_C;
    bool pc = load && (list & 0x8000);
    if (rn == 15 || n == 0)
        return -1;
    mov_rm(0, RAX, RG(rn));
    if (db)
        alu_ri(0, ALU_SUB, RAX, (int32_t)(4 * n));
    emit_page(k, load ? MO(rpage_plain) : MO(wpage), 3, 4 * n);
    lea(1, RDX, xmi(RDX, RAX, 1, 0));
    if (pc) {
        mov_rm(0, R9, xm(RDX, 4 * (int32_t)(n - 1)));
        alu_ri(0, ALU_CMP, R9, (int32_t)0xff000000u);
        jcc_l(CC_AE, lab(&K.l_gen1[k]));
        test_ri(R9, 1);
        jcc_l(CC_E, lab(&K.l_gen1[k]));
    }
    int32_t off = 0;
    for (unsigned i = 0; i < 15; i++)
        if (list & (1u << i)) {
            if (load) {
                mov_rm(0, RCX, xm(RDX, off));
                mov_mr(0, RG(i), RCX);
            } else {
                mov_rm(0, RCX, RG(i));
                mov_mr(0, xm(RDX, off), RCX);
            }
            off += 4;
        }
    if (wb && !(load && (list & (1u << rn))))
        alu_mi(0, ALU_ADD, RG(rn), db ? -(int32_t)(4 * n) : (int32_t)(4 * n));
    if (!pc)
        return 1 + (int)n;
    mov_rr(0, RAX, R9);
    alu_ri(0, ALU_AND, RAX, -2);
    emit_to_eax(k, 3 + (int)n);
    return 3 + (int)n;
}

// Clears the other core's exclusive monitor if it covers the address in r8, as excl_clear_others.
static void emit_clear_other_monitor(void)
{
    int done = new_label();
    mov_rm(0, RCX, xm(RBX, CO(id)));
    alu_ri(0, ALU_XOR, RCX, 1);
    op_r(0, false, 0x69, RCX, RCX, false); // imul ecx, ecx, sizeof(cpu_t)
    e32((uint32_t)sizeof(cpu_t));
    lea(1, RCX, xmi(R15, RCX, 1, MO(core)));
    test_mi8(xm(RCX, CO(excl_valid)), 1);
    jcc_l(CC_E, done);
    mov_rm(0, RDX, xm(RCX, CO(excl_addr)));
    alu_rr(0, ALU_XOR, RDX, R8);
    test_ri(RDX, ~7u);
    jcc_l(CC_NE, done);
    op_m(0, false, 0xc6, 0, xm(RCX, CO(excl_valid)), 1, false);
    e8(0);
    bind(done);
}

// The exclusive and acquire/release loads and stores, and TBB and TBH.
static int emit_excl(int k, const dop_t *d)
{
    uint16_t hw1 = d->hw1, hw2 = d->hw2;
    unsigned rn = hw1 & 0xf, rt = (hw2 >> 12) & 0xf;
    unsigned size, rd = 16;
    bool load, excl;
    uint32_t imm = 0;
    if ((hw1 & 0xfff0) == 0xe850 || ((hw1 & 0xfff0) == 0xe840 && (hw2 & 0xf03f) != 0xf000)) {
        load = hw1 & 0x10;
        excl = true;
        size = 4;
        imm = (hw2 & 0xffu) << 2;
        if (!load)
            rd = (hw2 >> 8) & 0xf;
    } else if ((hw1 & 0xfff0) == 0xe8c0 || (hw1 & 0xfff0) == 0xe8d0) {
        load = hw1 & 0x10;
        unsigned op3 = (hw2 >> 4) & 0xf;
        if (load && op3 <= 1) {
            // TBB and TBH.
            unsigned rm = hw2 & 0xf;
            uint32_t pc4 = d->pc + 4;
            // Every read through an uncached alias misses, which the handler charges.
            if (rm == 15 || (rn == 15 && (d->pc >> 28) == 1 && (d->pc >> 24) >= 0x14))
                return -1;
            mov_rm(0, RCX, RG(rm));
            if (op3)
                alu_rr(0, ALU_ADD, RCX, RCX);
            if (rn != 15) {
                alu_rm(0, ALU_ADD, RCX, RG(rn));
                mov_rr(0, RAX, RCX);
                emit_load_value(k, op3 ? 2 : 1, false);
            } else {
                // A table just after the instruction, so in the block's own page.
                alu_ri(0, ALU_ADD, RCX, (int32_t)pc4);
                mov_rr(0, RAX, RCX);
                shift_ri(SH_SHR, RAX, 16);
                alu_ri(0, ALU_CMP, RAX, (int32_t)(d->pc >> 16));
                jcc_l(CC_NE, lab(&K.l_gen1[k]));
                if (op3) {
                    test_ri(RCX, 1);
                    jcc_l(CC_NE, lab(&K.l_gen1[k]));
                }
                if ((d->pc >> 28) == 1) {
                    test_mi8(xm(R15, MO(xip_ctrl)), 1);
                    jcc_l(CC_E, lab(&K.l_gen1[k]));
                    mov_rr(0, RAX, RCX);
                    alu_ri(0, ALU_AND, RAX, 0x01ffffff);
                    shift_ri(SH_SHR, RAX, 3);
                    mov_rr(0, RDX, RAX);
                    alu_ri(0, ALU_AND, RDX, 1023);
                    shift_ri(SH_SHR, RAX, 10);
                    alu_ri(0, ALU_OR, RAX, (int32_t)0x80000000u);
                    alu_rm(0, ALU_CMP, RAX, xmi(R15, RDX, 4, MO(xip_mru)));
                    jcc_l(CC_NE, lab(&K.l_gen1[k]));
                    alu_mi(0, ALU_ADD, xm(R15, MO(xip_ctr_acc)), 1);
                    alu_mi(0, ALU_ADD, xm(R15, MO(xip_ctr_hit)), 1);
                }
                op_r(0, false, 0x0fb7, RCX, RCX, false); // movzx ecx, cx
                mov_ri64(RDX, (uint64_t)(uintptr_t)K.m->rpage[d->pc >> 16]);
                op_m(0, false, op3 ? 0x0fb7 : 0x0fb6, RAX, xmi(RDX, RCX, 1, 0), 0, false);
            }
            lea(0, RAX, xmi(RAX, RAX, 1, (int32_t)pc4));
            emit_to_eax(k, 4);
            return 4;
        }
        size = (op3 & 3) == 0 ? 1 : (op3 & 3) == 1 ? 2 : 4;
        excl = op3 & 4;
        if (!load && excl)
            rd = hw2 & 0xf;
    } else {
        return -1;
    }
    if (rn == 15 || rt == 15 || rd == 15)
        return -1;
    mov_rm(0, RAX, RG(rn));
    if (imm)
        alu_ri(0, ALU_ADD, RAX, (int32_t)imm);
    mov_rr(0, R8, RAX);
    if (load) {
        emit_load_value(k, (int)size, false);
        mov_mr(0, RG(rt), RAX);
        if (excl) {
            op_m(0, false, 0xc6, 0, xm(RBX, CO(excl_valid)), 1, false);
            e8(1);
            mov_mr(0, xm(RBX, CO(excl_addr)), R8);
        }
        return 2;
    }
    int fail = -1, done = -1;
    if (excl) {
        fail = new_label();
        done = new_label();
        test_mi8(xm(RBX, CO(excl_valid)), 1);
        jcc_l(CC_E, fail);
        alu_rm(0, ALU_CMP, R8, xm(RBX, CO(excl_addr)));
        jcc_l(CC_NE, fail);
    }
    emit_store_value(k, (int)size, rt);
    emit_clear_other_monitor();
    if (excl) {
        mov_mi(RG(rd), 0);
        op_m(0, false, 0xc6, 0, xm(RBX, CO(excl_valid)), 1, false);
        e8(0);
        jmp_l(done);
        // A failed store-exclusive stores nothing, so costs a cycle less.
        bind(fail);
        mov_mi(RG(rd), 1);
        op_m(0, false, 0xc6, 0, xm(RBX, CO(excl_valid)), 1, false);
        e8(0);
        alu_rm(1, ALU_SUB, R12, PSK(1));
        bind(done);
    }
    return 2;
}

// Data processing with a shifted register, where the flags it sets do not need the shifter's carry.
static int emit_dp_shifted(int k, const dop_t *d)
{
    uint16_t hw1 = d->hw1, hw2 = d->hw2;
    unsigned op = (hw1 >> 5) & 0xf, rn = hw1 & 0xf, rd = (hw2 >> 8) & 0xf, rm = hw2 & 0xf;
    bool s = hw1 & 0x10;
    unsigned type = (hw2 >> 4) & 3, imm5 = ((hw2 >> 10) & 0x1c) | ((hw2 >> 6) & 3);
    bool logical = op <= 4, arith = op == 8 || op == 0xa || op == 0xb || op == 0xd || op == 0xe;
    if ((!logical && !arith) || rm == 15)
        return -1;
    if (rn == 15 && op != 2 && op != 3)
        return -1;
    if (rd == 15 && !(s && (op == 0 || op == 4 || op == 8 || op == 0xd)))
        return -1;
    if (s && logical && (type || imm5))
        return -1;
    bool sub = op == 0xb || op == 0xd || op == 0xe;
    if (s && arith)
        alu_rr(0, ALU_XOR, RDX, RDX);
    mov_rm(0, RCX, RG(rm));
    unsigned n = (type == 1 || type == 2) && !imm5 ? 32 : imm5;
    if (type == 0 && n) {
        shift_ri(SH_SHL, RCX, (int)n);
    } else if (type == 1) {
        if (n == 32)
            alu_rr(0, ALU_XOR, RCX, RCX);
        else
            shift_ri(SH_SHR, RCX, (int)n);
    } else if (type == 2) {
        shift_ri(SH_SAR, RCX, n == 32 ? 31 : (int)n);
    } else if (type == 3) {
        if (n) {
            shift_ri(1, RCX, (int)n); // ror
        } else {
            bt_mi(APSR, 29);
            op_r(0, false, 0xd1, 3, RCX, false); // rcr ecx, 1
        }
    }
    if (rn == 15) {
        if (op == 3)
            not_r(RCX);
        mov_rr(0, RAX, RCX);
        if (s)
            test_rr(RAX, RAX);
    } else if (logical) {
        if (op == 1 || op == 3)
            not_r(RCX);
        mov_rm(0, RAX, RG(rn));
        alu_rr(0, op <= 1 ? ALU_AND : op == 4 ? ALU_XOR : ALU_OR, RAX, RCX);
    } else if (op == 0xe) {
        alu_rm(0, ALU_SUB, RCX, RG(rn));
        mov_rr(0, RAX, RCX);
    } else {
        if (op == 0xa || op == 0xb) {
            bt_mi(APSR, 29);
            if (op == 0xb)
                e8(0xf5);
        }
        mov_rm(0, RAX, RG(rn));
        alu_rr(0, op == 8 ? ALU_ADD : op == 0xa ? ALU_ADC : op == 0xb ? ALU_SBB : ALU_SUB, RAX, RCX);
    }
    if (rd != 15)
        mov_mr(0, RG(rd), RAX);
    if (s) {
        if (logical)
            emit_flags_nz(-1);
        else
            emit_flags_arith(sub);
    }
    return 1;
}

static void cmov_rr(int cc, int d, int s2)
{
    op_r(0, false, 0x0f40 + cc, d, s2, false);
}

// Shifts by a register without flags, the extends, REV, REV16, REVSH and CLZ.
static int emit_dp_register(int k, const dop_t *d)
{
    uint16_t hw1 = d->hw1, hw2 = d->hw2;
    unsigned op1 = (hw1 >> 4) & 0xf, op2 = (hw2 >> 4) & 0xf;
    unsigned rn = hw1 & 0xf, rd = (hw2 >> 8) & 0xf, rm = hw2 & 0xf;
    if ((hw2 & 0xf000) != 0xf000 || rd == 15 || rm == 15)
        return -1;
    if (op2 == 0 && op1 < 8) {
        if ((op1 & 1) || rn == 15)
            return -1;
        unsigned type = op1 >> 1;
        mov_rm(0, RCX, RG(rm));
        alu_ri(0, ALU_AND, RCX, 0xff);
        mov_rm(0, RAX, RG(rn));
        if (type <= 1) {
            alu_rr(0, ALU_XOR, RDX, RDX);
            op_r(0, false, 0xd3, type ? SH_SHR : SH_SHL, RAX, false);
            alu_ri(0, ALU_CMP, RCX, 32);
            cmov_rr(CC_AE, RAX, RDX);
        } else if (type == 2) {
            mov_ri(RDX, 31);
            alu_ri(0, ALU_CMP, RCX, 31);
            cmov_rr(CC_A, RCX, RDX);
            op_r(0, false, 0xd3, SH_SAR, RAX, false);
        } else {
            op_r(0, false, 0xd3, 1, RAX, false); // ror eax, cl
        }
        mov_mr(0, RG(rd), RAX);
        return 1;
    }
    if (op1 < 8 && (op2 & 8)) {
        if (op1 != 0 && op1 != 1 && op1 != 4 && op1 != 5)
            return -1;
        unsigned rot = (hw2 >> 4) & 3;
        mov_rm(0, RAX, RG(rm));
        if (rot)
            shift_ri(1, RAX, 8 * (int)rot);
        static const unsigned ext[6] = {0x0fbf, 0x0fb7, 0, 0, 0x0fbe, 0x0fb6};
        op_r(0, false, ext[op1], RAX, RAX, false);
        if (rn != 15)
            alu_rm(0, ALU_ADD, RAX, RG(rn));
        mov_mr(0, RG(rd), RAX);
        return 1;
    }
    if ((op1 & 0xc) == 8 && (op2 & 0xc) == 8) {
        unsigned o1 = op1 & 3, o2 = op2 & 3;
        if (!(o1 == 3 || (o1 == 1 && o2 != 2)))
            return -1;
        mov_rm(0, RAX, RG(rm));
        if (o1 == 1 && o2 == 0) {
            op_r(0, false, 0x0fc8, 0, RAX, false); // bswap eax
        } else if (o1 == 1 && o2 == 1) {
            mov_rr(0, RCX, RAX);
            shift_ri(SH_SHR, RAX, 8);
            alu_ri(0, ALU_AND, RAX, 0x00ff00ff);
            shift_ri(SH_SHL, RCX, 8);
            alu_ri(0, ALU_AND, RCX, (int32_t)0xff00ff00u);
            alu_rr(0, ALU_OR, RAX, RCX);
        } else if (o1 == 1 && o2 == 3) {
            op_r(0, true, 0xc1, 0, RAX, false); // rol ax, 8
            e8(8);
            op_r(0, false, 0x0fbf, RAX, RAX, false);
        } else if (o1 == 3) {
            mov_ri(RDX, 0xffffffffu);
            op_r(0, false, 0x0fbd, RCX, RAX, false); // bsr ecx, eax
            cmov_rr(CC_E, RCX, RDX);
            mov_ri(RAX, 31);
            alu_rr(0, ALU_SUB, RAX, RCX);
        } else {
            return -1;
        }
        mov_mr(0, RG(rd), RAX);
        return 1;
    }
    return -1;
}

// ADDW, SUBW, MOVW, MOVT, the bitfield extracts, BFI and BFC.
static int emit_dp_plain(int k, const dop_t *d)
{
    uint16_t hw1 = d->hw1, hw2 = d->hw2;
    unsigned op = (hw1 >> 4) & 0x1f, rn = hw1 & 0xf, rd = (hw2 >> 8) & 0xf;
    uint32_t imm12 = ((uint32_t)(hw1 & 0x400) << 1) | ((hw2 >> 4) & 0x700) | (hw2 & 0xff);
    unsigned imm5 = ((hw2 >> 10) & 0x1c) | ((hw2 >> 6) & 3), widthm1 = hw2 & 0x1f;
    if (rd == 15)
        return -1;
    switch (op) {
    case 0x00:
    case 0x0a:
        if (rn == 15) {
            uint32_t base = (d->pc + 4) & ~3u;
            mov_mi(RG(rd), op ? base - imm12 : base + imm12);
            return 1;
        }
        mov_rm(0, RAX, RG(rn));
        alu_ri(0, op ? ALU_SUB : ALU_ADD, RAX, (int32_t)imm12);
        mov_mr(0, RG(rd), RAX);
        return 1;
    case 0x04:
        mov_mi(RG(rd), ((uint32_t)(hw1 & 0xf) << 12) | imm12);
        return 1;
    case 0x0c:
        mov_rm(0, RAX, RG(rd));
        op_r(0, false, 0x0fb7, RAX, RAX, false);
        alu_ri(0, ALU_OR, RAX, (int32_t)((((uint32_t)(hw1 & 0xf) << 12) | imm12) << 16));
        mov_mr(0, RG(rd), RAX);
        return 1;
    case 0x14:
    case 0x1c: {
        if (rn == 15)
            return -1;
        unsigned w = widthm1 + 1;
        mov_rm(0, RAX, RG(rn));
        if (imm5)
            shift_ri(SH_SHR, RAX, (int)imm5);
        if (w < 32) {
            if (op == 0x14) {
                shift_ri(SH_SHL, RAX, 32 - (int)w);
                shift_ri(SH_SAR, RAX, 32 - (int)w);
            } else {
                alu_ri(0, ALU_AND, RAX, (int32_t)((1u << w) - 1));
            }
        }
        mov_mr(0, RG(rd), RAX);
        return 1;
    }
    case 0x16: {
        unsigned lsb = imm5, msb = widthm1;
        if (msb < lsb)
            return 1;
        uint32_t mask = (uint32_t)((((uint64_t)1 << (msb - lsb + 1)) - 1) << lsb);
        if (rn == 15) {
            alu_mi(0, ALU_AND, RG(rd), (int32_t)~mask);
            return 1;
        }
        mov_rm(0, RAX, RG(rn));
        if (lsb)
            shift_ri(SH_SHL, RAX, (int)lsb);
        alu_ri(0, ALU_AND, RAX, (int32_t)mask);
        mov_rm(0, RCX, RG(rd));
        alu_ri(0, ALU_AND, RCX, (int32_t)~mask);
        alu_rr(0, ALU_OR, RAX, RCX);
        mov_mr(0, RG(rd), RAX);
        return 1;
    }
    default:
        return -1;
    }
}

// MUL, MLA and MLS; SMULL, UMULL, SMLAL and UMLAL; SDIV and UDIV.
static int emit_multiply(int k, const dop_t *d, bool lng)
{
    uint16_t hw1 = d->hw1, hw2 = d->hw2;
    unsigned op1 = (hw1 >> 4) & 7, rn = hw1 & 0xf, rm = hw2 & 0xf;
    if (rn == 15 || rm == 15)
        return -1;
    if (!lng) {
        unsigned op2 = (hw2 >> 4) & 3, ra = (hw2 >> 12) & 0xf, rd = (hw2 >> 8) & 0xf;
        if (op1 || op2 > 1 || rd == 15 || (op2 == 1 && ra == 15))
            return -1;
        mov_rm(0, RAX, RG(rn));
        imul_rm(0, RAX, RG(rm));
        if (op2 == 1) {
            mov_rm(0, RCX, RG(ra));
            alu_rr(0, ALU_SUB, RCX, RAX);
            mov_rr(0, RAX, RCX);
        } else if (ra != 15) {
            alu_rm(0, ALU_ADD, RAX, RG(ra));
        }
        mov_mr(0, RG(rd), RAX);
        return 1;
    }
    unsigned op2 = (hw2 >> 4) & 0xf, rlo = (hw2 >> 12) & 0xf, rhi = (hw2 >> 8) & 0xf;
    if (rhi == 15 || (rlo == 15 && op1 != 1 && op1 != 3))
        return -1;
    if (op1 == 1 || op1 == 3) {
        if (op2 != 0xf)
            return -1;
        // Division by zero, and the one signed overflow, go to the handler.
        mov_rm(0, RCX, RG(rm));
        test_rr(RCX, RCX);
        jcc_l(CC_E, lab(&K.l_gen1[k]));
        mov_rm(0, RAX, RG(rn));
        if (op1 == 1) {
            int ok = new_label();
            alu_ri(0, ALU_CMP, RAX, (int32_t)0x80000000u);
            jcc_l(CC_NE, ok);
            alu_ri(0, ALU_CMP, RCX, -1);
            jcc_l(CC_E, lab(&K.l_gen1[k]));
            bind(ok);
            e8(0x99); // cdq
            op_r(0, false, 0xf7, 7, RCX, false); // idiv ecx
        } else {
            alu_rr(0, ALU_XOR, RDX, RDX);
            op_r(0, false, 0xf7, 6, RCX, false); // div ecx
        }
        mov_mr(0, RG(rhi), RAX);
        return 5;
    }
    if (op2 != 0 || !(op1 == 0 || op1 == 2 || op1 == 4 || op1 == 6))
        return -1;
    mov_rm(0, RAX, RG(rn));
    op_m(0, false, 0xf7, (op1 & 2) ? 4 : 5, RG(rm), 0, false); // mul or imul edx:eax
    if (op1 >= 4) {
        alu_rm(0, ALU_ADD, RAX, RG(rlo));
        alu_rm(0, ALU_ADC, RDX, RG(rhi));
    }
    mov_mr(0, RG(rlo), RAX);
    mov_mr(0, RG(rhi), RDX);
    return 2;
}

static int emit_inline(int k, const dop_t *d, int *taken_extra);

static inline uint32_t it_next(uint32_t it)
{
    return (it & 7) ? (it & 0xe0) | ((it << 1) & 0x1f) : 0;
}

// An instruction under IT: skipped unless its condition holds, otherwise done as outside IT, but
// with the 16-bit forms setting no flags. The IT state moves on first, so that instructions which
// end the block need not; the handler path puts it back.
static int emit_it_op(int k, const dop_t *d, int *taken_extra)
{
    unsigned cond = K.it[k] >> 4;
    bool wide = (d->lf & LF_LEN) == 4;
    if (cond >= 0xe || (d->lf & FETCH_HW2))
        return -1;
    dop_t t = *d;
    if (!wide)
        decode16(&t, d->pc, d->hw1);
    if (t.fn == h_bcond || t.fn == h_cbz)
        return -1;
    uint8_t *mark = J.p;
    int fixmark = nfix;
    mov_rm(0, RAX, APSR);
    emit_cond_jump(cond ^ 1, lab(&K.l_skip[k]));
    mov_mi(xm(RBX, CO(itstate)), it_next(K.it[k]));
    K.nf = !wide;
    int st = emit_inline(k, &t, taken_extra);
    K.nf = false;
    if (st < 0) {
        J.p = mark;
        nfix = fixmark;
        K.l_skip[k] = -1;
        return -1;
    }
    return st;
}

// Native code for instruction k, returning its fixed cycle count, or -1 to use the handler, in
// which case nothing has been emitted. *taken gets the extra cycles of a taken branch.
static int emit_inline(int k, const dop_t *d, int *taken_extra)
{
    dop_fn fn = d->fn;
    bool last = k == K.n - 1;
    *taken_extra = 0;

    if (K.it[k] && d == &K.b->ops[k])
        return emit_it_op(k, d, taken_extra);
    if (fn == h_nop)
        return 1;
    if (fn == h_exec16 && is_it(d->hw1) && !last) {
        mov_mi(xm(RBX, CO(itstate)), d->hw1 & 0xff);
        return 1;
    }
    int sys = emit_system(k, d);
    if (sys >= 0)
        return sys;
    if (fn == h_ldst_single)
        return emit_ldst_single(k, d);
    if (fn == h_ldm_stm)
        return emit_ldm_stm(k, d);
    if (fn == h_ldst_dual_excl)
        return emit_excl(k, d);
    if (fn == h_dp_shifted_reg)
        return emit_dp_shifted(k, d);
    if (fn == h_dp_register)
        return emit_dp_register(k, d);
    if (fn == h_dp_plain_imm)
        return emit_dp_plain(k, d);
    if (fn == h_multiply || fn == h_long_multiply)
        return emit_multiply(k, d, fn == h_long_multiply);

    // In an IT block (K.nf), the 16-bit data-processing forms leave the flags alone, apart from
    // the comparisons.
    if (fn == h_movs_reg) {
        mov_rm(0, RAX, RG(d->rm));
        mov_mr(0, RG(d->rd), RAX);
        if (!K.nf) {
            test_rr(RAX, RAX);
            emit_flags_nz(-1);
        }
        return 1;
    }
    if (fn == h_lsl_imm || ((fn == h_lsr_imm || fn == h_asr_imm) && d->imm)) {
        mov_rm(0, RAX, RG(d->rm));
        shift_ri(fn == h_lsl_imm ? SH_SHL : fn == h_lsr_imm ? SH_SHR : SH_SAR, RAX, (int)d->imm);
        mov_mr(0, RG(d->rd), RAX);
        if (!K.nf)
            emit_flags_nzc();
        return 1;
    }
    if (fn == h_add_reg || fn == h_sub_reg || fn == h_add_imm || fn == h_sub_imm || fn == h_cmp_imm ||
        fn == h_cmp_reg) {
        bool sub = fn != h_add_reg && fn != h_add_imm;
        bool reg = fn == h_add_reg || fn == h_sub_reg || fn == h_cmp_reg;
        bool store = fn != h_cmp_imm && fn != h_cmp_reg;
        alu_rr(0, ALU_XOR, RDX, RDX);
        mov_rm(0, RAX, RG(d->rn));
        int op = store ? (sub ? ALU_SUB : ALU_ADD) : ALU_CMP;
        if (reg)
            alu_rm(0, op, RAX, RG(d->rm));
        else
            alu_ri(0, op, RAX, (int32_t)d->imm);
        if (store)
            mov_mr(0, RG(d->rd), RAX);
        if (!K.nf || !store)
            emit_flags_arith(sub);
        return 1;
    }
    if (fn == h_movs_imm) {
        mov_mi(RG(d->rd), d->imm);
        if (!K.nf)
            emit_flags_const(d->imm, -1);
        return 1;
    }
    if (fn == h_alu) {
        unsigned rdn = d->rd, rm = d->rm;
        switch (d->imm) {
        case 0x0:
        case 0x1:
        case 0x8:
        case 0xc:
            mov_rm(0, RAX, RG(rdn));
            alu_rm(0, d->imm == 0x1 ? ALU_XOR : d->imm == 0xc ? ALU_OR : ALU_AND, RAX, RG(rm));
            if (d->imm != 0x8)
                mov_mr(0, RG(rdn), RAX);
            if (!K.nf || d->imm == 0x8)
                emit_flags_nz(-1);
            return 1;
        case 0x5:
        case 0x6:
            alu_rr(0, ALU_XOR, RDX, RDX);
            bt_mi(APSR, 29);
            if (d->imm == 0x6)
                e8(0xf5); // cmc: x86 borrows where ARM's carry is clear
            mov_rm(0, RAX, RG(rdn));
            alu_rm(0, d->imm == 0x5 ? ALU_ADC : ALU_SBB, RAX, RG(rm));
            mov_mr(0, RG(rdn), RAX);
            if (!K.nf)
                emit_flags_arith(d->imm == 0x6);
            return 1;
        case 0x9:
            alu_rr(0, ALU_XOR, RDX, RDX);
            alu_rr(0, ALU_XOR, RAX, RAX);
            alu_rm(0, ALU_SUB, RAX, RG(rm));
            mov_mr(0, RG(rdn), RAX);
            if (!K.nf)
                emit_flags_arith(true);
            return 1;
        case 0xb:
            alu_rr(0, ALU_XOR, RDX, RDX);
            mov_rm(0, RAX, RG(rdn));
            alu_rm(0, ALU_ADD, RAX, RG(rm));
            emit_flags_arith(false);
            return 1;
        case 0xd:
            mov_rm(0, RAX, RG(rdn));
            imul_rm(0, RAX, RG(rm));
            mov_mr(0, RG(rdn), RAX);
            if (!K.nf) {
                test_rr(RAX, RAX);
                emit_flags_nz(-1);
            }
            return 1;
        case 0xe:
            mov_rm(0, RCX, RG(rm));
            not_r(RCX);
            mov_rm(0, RAX, RG(rdn));
            alu_rr(0, ALU_AND, RAX, RCX);
            mov_mr(0, RG(rdn), RAX);
            if (!K.nf)
                emit_flags_nz(-1);
            return 1;
        case 0xf:
            mov_rm(0, RAX, RG(rm));
            not_r(RAX);
            mov_mr(0, RG(rdn), RAX);
            if (!K.nf) {
                test_rr(RAX, RAX);
                emit_flags_nz(-1);
            }
            return 1;
        default:
            return -1;
        }
    }
    if (fn == h_add_hi) {
        mov_rm(0, RAX, RG(d->rm));
        alu_mr(0, ALU_ADD, RG(d->rd), RAX);
        return 1;
    }
    if (fn == h_mov_hi) {
        mov_rm(0, RAX, RG(d->rm));
        mov_mr(0, RG(d->rd), RAX);
        return 1;
    }
    if (fn == h_set_const) {
        mov_mi(RG(d->rd), d->imm);
        return 1;
    }
    if (fn == h_add_sp_imm) {
        emit_addr_imm(13, d->imm);
        mov_mr(0, RG(d->rd), RAX);
        return 1;
    }
    if (fn == h_adjust_sp) {
        alu_mi(0, ALU_ADD, RG(13), (int32_t)d->imm);
        return 1;
    }
    if (fn == h_extend) {
        static const unsigned opc[4] = {0x0fbf, 0x0fbe, 0x0fb7, 0x0fb6};
        op_m(0, false, opc[d->imm & 3], RAX, RG(d->rm), 0, false);
        mov_mr(0, RG(d->rd), RAX);
        return 1;
    }
    if (fn == h_ldr_lit) {
        uint32_t a = d->imm, pc0 = K.b->tag & ~1u;
        if ((a >> 16) != (pc0 >> 16) || (a & 3))
            return -1;
        const uint8_t *page = K.m->rpage[a >> 16];
        if (!page)
            return -1;
        if (K.fixed) {
            // ROM and flash hold still for as long as the block is current.
            if ((a >> 28) == 1)
                emit_xip_hit(a, lab(&K.l_gen1[k]));
            mov_mi(RG(d->rd), rd32le(page + (a & 0xffff)));
        } else {
            mov_rm(0, RAX, xm(R15, MO(sram) + (int32_t)(a - SRAM_BASE)));
            mov_mr(0, RG(d->rd), RAX);
        }
        return 2;
    }
    if (fn == h_ldr_imm || fn == h_ldrh_imm || fn == h_ldrb_imm || fn == h_ldr_w || fn == h_ldrh_w ||
        fn == h_ldrb_w) {
        int size = (fn == h_ldr_imm || fn == h_ldr_w) ? 4 : (fn == h_ldrh_imm || fn == h_ldrh_w) ? 2 : 1;
        K.mm[k].size = (uint8_t)size;
        K.mm[k].reg = d->rd;
        K.mm[k].load = true;
        K.mm[k].keep_on_fault = fn == h_ldr_w || fn == h_ldrh_w || fn == h_ldrb_w;
        emit_addr_imm(d->rn, d->imm);
        emit_load_value(k, size, false);
        mov_mr(0, RG(d->rd), RAX);
        return 2;
    }
    if (fn == h_str_imm || fn == h_strh_imm || fn == h_strb_imm) {
        int size = fn == h_str_imm ? 4 : fn == h_strh_imm ? 2 : 1;
        K.mm[k].size = (uint8_t)size;
        K.mm[k].reg = d->rd;
        emit_addr_imm(d->rn, d->imm);
        emit_store_value(k, size, d->rd);
        return 2;
    }
    if (fn == h_ldst_reg) {
        static const int8_t size[8] = {4, 2, 1, 1, 4, 2, 1, 2};
        K.mm[k].size = (uint8_t)size[d->imm];
        K.mm[k].reg = d->rd;
        K.mm[k].load = d->imm >= 3;
        K.mm[k].sign = d->imm == 3 || d->imm == 7;
        mov_rm(0, RAX, RG(d->rn));
        alu_rm(0, ALU_ADD, RAX, RG(d->rm));
        if (d->imm < 3) {
            emit_store_value(k, size[d->imm], d->rd);
        } else {
            emit_load_value(k, size[d->imm], d->imm == 3 || d->imm == 7);
            mov_mr(0, RG(d->rd), RAX);
        }
        return 2;
    }
    if (fn == h_ldr_w8 || fn == h_str_w8) {
        bool index = d->lf & LF_A, wb = d->lf & LF_B;
        emit_addr_imm(d->rn, index ? d->imm : 0);
        if (fn == h_ldr_w8)
            emit_load_value(k, 4, false);
        else
            emit_store_value(k, 4, d->rd);
        if (wb) {
            mov_rm(0, RCX, RG(d->rn));
            alu_ri(0, ALU_ADD, RCX, (int32_t)d->imm);
            mov_mr(0, RG(d->rn), RCX);
        }
        if (fn == h_ldr_w8)
            mov_mr(0, RG(d->rd), RAX);
        return 2;
    }
    if (fn == h_ldrd_strd) {
        bool index = d->lf & LF_A, wb = d->lf & LF_B, load = d->lf & LF_C;
        if (d->rd == 15 || d->rm == 15 || (wb && (d->rd == d->rn || d->rm == d->rn)))
            return -1;
        emit_addr_imm(d->rn, index ? d->imm : 0);
        emit_page(k, load ? MO(rpage_plain) : MO(wpage), 3, 8);
        if (load) {
            mov_rm(0, RCX, xmi(RDX, RAX, 1, 0));
            mov_rm(0, RAX, xmi(RDX, RAX, 1, 4));
            mov_mr(0, RG(d->rd), RCX);
            mov_mr(0, RG(d->rm), RAX);
        } else {
            mov_rm(0, RCX, RG(d->rd));
            mov_mr(0, xmi(RDX, RAX, 1, 0), RCX);
            mov_rm(0, RCX, RG(d->rm));
            mov_mr(0, xmi(RDX, RAX, 1, 4), RCX);
        }
        if (wb) {
            mov_rm(0, RCX, RG(d->rn));
            alu_ri(0, ALU_ADD, RCX, (int32_t)d->imm);
            mov_mr(0, RG(d->rn), RCX);
        }
        return 3;
    }
    if (fn == h_push) {
        unsigned n = d->rm;
        mov_rm(0, RAX, RG(13));
        alu_ri(0, ALU_SUB, RAX, (int32_t)(4 * n));
        mov_rr(0, R8, RAX);
        emit_page(k, MO(wpage), 3, 4 * n);
        mov_mr(0, RG(13), R8);
        int32_t off = 0;
        for (unsigned i = 0; i < 15; i++)
            if (d->imm & (1u << i)) {
                mov_rm(0, RCX, RG(i));
                mov_mr(0, xmi(RDX, RAX, 1, off), RCX);
                off += 4;
            }
        return 1 + (int)n;
    }
    if (fn == h_pop) {
        unsigned list = d->imm & 0xff, nl = (unsigned)__builtin_popcount(list);
        bool pc = d->imm & 0x100;
        unsigned n = nl + (pc ? 1 : 0);
        mov_rm(0, RAX, RG(13));
        emit_page(k, MO(rpage_plain), 3, 4 * n);
        if (pc) {
            mov_rm(0, R9, xmi(RDX, RAX, 1, 4 * (int32_t)nl));
            alu_ri(0, ALU_CMP, R9, (int32_t)0xff000000u);
            jcc_l(CC_AE, lab(&K.l_gen1[k]));
            test_ri(R9, 1);
            jcc_l(CC_E, lab(&K.l_gen1[k]));
        }
        int32_t off = 0;
        for (unsigned i = 0; i < 8; i++)
            if (list & (1u << i)) {
                mov_rm(0, RCX, xmi(RDX, RAX, 1, off));
                mov_mr(0, RG(i), RCX);
                off += 4;
            }
        alu_mi(0, ALU_ADD, RG(13), (int32_t)(4 * n));
        if (!pc)
            return 1 + (int)n;
        mov_rr(0, RAX, R9);
        alu_ri(0, ALU_AND, RAX, -2);
        emit_to_eax(k, 3 + (int)n);
        return 3 + (int)n;
    }
    if (fn == h_dp_imm) {
        unsigned op = d->rm, rd = d->rd, rn = d->rn;
        bool s = d->lf & LF_A;
        int carry = (d->lf & LF_B) ? (int)(d->imm >> 31) : -1;
        uint32_t imm = d->imm;
        switch (op) {
        case 0x0:
        case 0x1:
        case 0x2:
        case 0x3:
        case 0x4: {
            bool test_ok = op == 0x0 || op == 0x4;
            if ((rd == 15 && !test_ok) || (rn == 15 && op != 0x2 && op != 0x3))
                return -1;
            if (rn == 15) {
                uint32_t v = op == 0x2 ? imm : ~imm;
                mov_mi(RG(rd), v);
                if (s)
                    emit_flags_const(v, carry);
                return 1;
            }
            mov_rm(0, RAX, RG(rn));
            int aop = (op == 0x0 || op == 0x1) ? ALU_AND : (op == 0x4) ? ALU_XOR : ALU_OR;
            uint32_t b = (op == 0x1 || op == 0x3) ? ~imm : imm;
            alu_ri(0, aop, RAX, (int32_t)b);
            if (rd != 15)
                mov_mr(0, RG(rd), RAX);
            if (s)
                emit_flags_nz(carry);
            return 1;
        }
        case 0x8:
        case 0xd:
        case 0xa:
        case 0xb:
        case 0xe: {
            if (rn == 15 || (rd == 15 && op != 0x8 && op != 0xd))
                return -1;
            bool sub = op == 0xd || op == 0xb || op == 0xe;
            if (s)
                alu_rr(0, ALU_XOR, RDX, RDX);
            if (op == 0xe) {
                mov_ri(RAX, imm);
                alu_rm(0, ALU_SUB, RAX, RG(rn));
            } else {
                if (op == 0xa || op == 0xb) {
                    bt_mi(APSR, 29);
                    if (op == 0xb)
                        e8(0xf5);
                }
                mov_rm(0, RAX, RG(rn));
                static const int aops[16] = {[0x8] = ALU_ADD, [0xa] = ALU_ADC, [0xb] = ALU_SBB, [0xd] = ALU_SUB};
                alu_ri(0, aops[op], RAX, (int32_t)imm);
            }
            if (rd != 15)
                mov_mr(0, RG(rd), RAX);
            if (s)
                emit_flags_arith(sub);
            return 1;
        }
        default:
            return -1;
        }
    }

    // Branches, which end the block.
    if (!last)
        return -1;
    if (fn == h_bcond || fn == h_cbz) {
        int taken = new_label();
        if (fn == h_bcond) {
            mov_rm(0, RAX, APSR);
            emit_cond_jump(d->rd, taken);
        } else {
            alu_mi(0, ALU_CMP, RG(d->rn), 0);
            jcc_l(d->rd ? CC_NE : CC_E, taken);
        }
        emit_two_way(k, 1, taken, d->imm);
        *taken_extra = 1;
        return 1;
    }
    if (fn == h_branch || fn == h_bl) {
        int st = fn == h_bl ? 3 : 2;
        if (fn == h_bl)
            mov_mi(RG(14), (d->pc + 4) | 1);
        alu_rm(1, ALU_ADD, R12, PSK(K.sb[k] + st));
        emit_add_insns(K.n);
        emit_link(d->imm);
        K.ended = true;
        return st;
    }
    if (fn == h_bx || fn == h_blx) {
        mov_rm(0, RAX, RG(d->rm));
        emit_check_target(k);
        if (fn == h_blx)
            mov_mi(RG(14), (d->pc + 2) | 1);
        emit_to_eax(k, 2);
        return 2;
    }
    return -1;
}

static void emit_block_entry(void)
{
    const block_t *b = K.b;
    uint32_t pc0 = b->tag & ~1u;
    if (K.fixed) {
        // The first instruction must be at least five bytes, for patching into a jump.
        xm_t g = xm(R15, MO(code_gen));
        g.d32 = true;
        op_m(0, false, 0x81, 7, g, 4, false);
        e32(b->gen);
        jcc_l(CC_NE, K.l_stale);
    } else {
        int32_t base = MO(sram) + (int32_t)(pc0 - SRAM_BASE);
        unsigned n = b->bytes, off = 0;
        if (n >= 8) {
            for (; off + 8 <= n; off += 8) {
                uint64_t v;
                memcpy(&v, b->code + off, 8);
                mov_ri64(RAX, v);
                alu_rm(1, ALU_CMP, RAX, xm(R15, base + (int32_t)off));
                jcc_l(CC_NE, K.l_stale);
            }
            if (off < n) {
                off = n - 8;
                uint64_t v;
                memcpy(&v, b->code + off, 8);
                mov_ri64(RAX, v);
                alu_rm(1, ALU_CMP, RAX, xm(R15, base + (int32_t)off));
                jcc_l(CC_NE, K.l_stale);
            }
        } else {
            for (; off < n; off += (n - off >= 4) ? 4 : 2) {
                xm_t at = xm(R15, base + (int32_t)off);
                at.d32 = true;
                if (n - off >= 4) {
                    op_m(0, false, 0x81, 7, at, 4, false);
                    e32(rd32le(b->code + off));
                } else {
                    op_m(0, true, 0x81, 7, at, 2, false);
                    e8(b->code[off]);
                    e8(b->code[off + 1]);
                }
                jcc_l(CC_NE, K.l_stale);
            }
        }
    }
    if (!K.careful) {
        emit_budget_check(0);
        K.pre_site = J.p - 4;
    }
}

static void emit_cold(void)
{
    const block_t *b = K.b;
    int n = K.n;

    emit_xip_ways();

    for (int k = 0; k < n; k++) {
        const dop_t *d = &b->ops[k];
        if (K.l_fetch1[k] >= 0) {
            bind(K.l_fetch1[k]);
            mov_rr(1, ARG0, R15);
            mov_ri(ARG1, d->pc);
            call_a((const void *)xip_access);
            alu_ri(0, ALU_ADD, RAX, 1);
            mov_mr(0, xm(RBX, CO(cyc)), RAX);
            mov_mi(xm(RBX, CO(fetch_line)), d->pc & ~7u);
            if ((d->lf & FETCH_HW2) && !K.it[k]) {
                mov_rr(1, ARG0, R15);
                mov_ri(ARG1, d->pc + 2);
                call_a((const void *)xip_access);
                alu_mr(0, ALU_ADD, xm(RBX, CO(cyc)), RAX);
                mov_mi(xm(RBX, CO(fetch_line)), (d->pc + 2) & ~7u);
            }
            jmp_l(lab(&K.l_gencyc[k]));
        }
        if (K.l_fetch2[k] >= 0) {
            bind(K.l_fetch2[k]);
            mov_rr(1, ARG0, R15);
            mov_ri(ARG1, d->pc + 2);
            call_a((const void *)xip_access);
            alu_ri(0, ALU_ADD, RAX, 1);
            mov_mr(0, xm(RBX, CO(cyc)), RAX);
            mov_mi(xm(RBX, CO(fetch_line)), (d->pc + 2) & ~7u);
            jmp_l(lab(&K.l_gencyc[k]));
        }
        if (K.l_other[k] >= 0) {
            bind(K.l_other[k]);
            unsigned lsize = K.ld[k].size;
            if (lsize) {
                // A load through a cached XIP alias that hits the line its set used last, which
                // ld32 and friends charge nothing for.
                int not_xip = new_label();
                mov_rr(0, RCX, RAX);
                shift_ri(SH_SHR, RCX, 26);
                alu_ri(0, ALU_CMP, RCX, 4);
                jcc_l(CC_NE, not_xip);
                if (lsize > 1) {
                    test_ri(RAX, lsize - 1);
                    jcc_l(CC_NE, lab(&K.l_gen1[k]));
                }
                test_mi8(xm(R15, MO(xip_ctrl)), 1);
                jcc_l(CC_E, lab(&K.l_gen1[k]));
                mov_rr(0, RCX, RAX);
                shift_ri(SH_SHR, RCX, 16);
                mov_rm(1, RDX, xmi(R15, RCX, 8, MO(rpage)));
                test_rr(RDX, RDX);
                jcc_l(CC_E, lab(&K.l_gen1[k]));
                mov_rr(0, RCX, RAX);
                alu_ri(0, ALU_AND, RCX, 0x01ffffff);
                shift_ri(SH_SHR, RCX, 3);
                mov_rr(0, R9, RCX);
                alu_ri(0, ALU_AND, R9, 1023);
                shift_ri(SH_SHR, RCX, 10);
                alu_ri(0, ALU_OR, RCX, (int32_t)0x80000000u);
                int hit = new_label(), way1 = new_label(), found = new_label();
                alu_rm(0, ALU_CMP, RCX, xmi(R15, R9, 4, MO(xip_mru)));
                jcc_l(CC_E, hit);
                alu_rm(0, ALU_CMP, RCX, xmi(R15, R9, 8, MO(xip_tag)));
                jcc_l(CC_NE, way1);
                op_m(0, false, 0xc6, 0, xmi(R15, R9, 1, MO(xip_lru)), 1, false);
                e8(1);
                jmp_l(found);
                bind(way1);
                alu_rm(0, ALU_CMP, RCX, xmi(R15, R9, 8, MO(xip_tag) + 4));
                jcc_l(CC_NE, lab(&K.l_gen1[k]));
                op_m(0, false, 0xc6, 0, xmi(R15, R9, 1, MO(xip_lru)), 1, false);
                e8(0);
                bind(found);
                mov_mr(0, xmi(R15, R9, 4, MO(xip_mru)), RCX);
                bind(hit);
                alu_mi(0, ALU_ADD, xm(R15, MO(xip_ctr_acc)), 1);
                alu_mi(0, ALU_ADD, xm(R15, MO(xip_ctr_hit)), 1);
                op_r(0, false, 0x0fb7, RAX, RAX, false);
                emit_host_load((int)lsize, K.ld[k].sign, xmi(RDX, RAX, 1, 0));
                jmp_l(K.l_loaded[k]);
                bind(not_xip);
            }
            if (K.mm[k].size)
                jmp_l(lab(&K.l_mmio[k]));
            else
                jmp_l(lab(&K.l_gen1[k]));
        }
        if (K.l_mmio[k] >= 0) {
            // As ld32, ld16, ld8 and their stores do for an aligned peripheral address.
            bind(K.l_mmio[k]);
            unsigned size = K.mm[k].size;
            if (size > 1) {
                test_ri(RAX, size - 1);
                jcc_l(CC_NE, lab(&K.l_gen1[k]));
            }
            alu_ri(0, ALU_CMP, RAX, 0x40000000);
            jcc_l(CC_B, lab(&K.l_gen1[k]));
            mov_mi(xm(RBX, CO(cyc)), 2);
            mov_mi(RG(15), op_next(d));
            mov_rr(0, ARG1, RAX);
            emit_store_time(k);
            mov_rr(1, ARG0, RBX);
            if (K.mm[k].load) {
                mov_ri(ARG2, size);
                call_a((const void *)bus_read);
                if (K.mm[k].sign)
                    op_r(0, false, size == 1 ? 0x0fbe : 0x0fbf, RAX, RAX, false);
                int skip = -1;
                if (K.mm[k].keep_on_fault) {
                    skip = new_label();
                    test_mi8(xm(RBX, CO(fault)), 1);
                    jcc_l(CC_NE, skip);
                }
                mov_mr(0, RG(K.mm[k].reg), RAX);
                if (skip >= 0)
                    bind(skip);
            } else {
                mov_rm(0, ARG2, RG(K.mm[k].reg));
                if (size == 2)
                    op_r(0, false, 0x0fb7, ARG2, ARG2, false);
                else if (size == 1)
                    op_r(0, false, 0x0fb6, ARG2, ARG2, ARG2 >= 4);
                mov_ri(ARG3, size);
                call_a((const void *)bus_write);
            }
            cmp16_mi(xm(RBX, CO(check_exc)), 0);
            jcc_l(CC_NE, lab(&K.l_look[k]));
            mov_rm(0, RAX, xm(RBX, CO(cyc)));
            imul_rm(1, RAX, PSK(1));
            alu_rr(1, ALU_ADD, R12, RAX);
            alu_rm(1, ALU_SUB, R12, PSK(K.sb[k + 1] - K.sb[k]));
            emit_budget_check(k + 1);
            jmp_l(k + 1 < K.n ? K.l_main[k + 1] : K.l_fall);
        }
        if (K.l_skip[k] >= 0) {
            // The condition failed: one cycle, and the IT state moves on.
            bind(K.l_skip[k]);
            mov_mi(xm(RBX, CO(itstate)), it_next(K.it[k]));
            int st = K.sb[k + 1] - K.sb[k];
            if (st > 1)
                alu_rm(1, ALU_SUB, R12, PSK(st - 1));
            jmp_l(K.l_post[k] >= 0 ? K.l_post[k] : K.l_fall);
        }
        if (!K.generic[k] && (K.l_gen1[k] >= 0 || K.l_gencyc[k] >= 0)) {
            bind(lab(&K.l_gen1[k]));
            if (K.it[k])
                mov_mi(xm(RBX, CO(itstate)), K.it[k]);
            mov_mi(xm(RBX, CO(cyc)), 1);
            bind(lab(&K.l_gencyc[k]));
            emit_generic_body(k, d, K.sb[k + 1] - K.sb[k], true);
        }
    }

    if (K.l_fall >= 0 && !K.fall_bound) {
        bind(K.l_fall);
        if (K.sb[n])
            alu_rm(1, ALU_ADD, R12, PSK(K.sb[n]));
        emit_add_insns(n);
        emit_link(op_next(&b->ops[n - 1]));
    }

    for (int k = 0; k <= n; k++) {
        if (K.l_careful[k] >= 0) {
            bind(K.l_careful[k]);
            mov_mi(RG(15), k < n ? b->ops[k].pc : op_next(&b->ops[n - 1]));
            if (K.sb[k])
                alu_rm(1, ALU_ADD, R12, PSK(K.sb[k]));
            emit_add_insns(k);
            if (K.careful) {
                jmp_a(J.exit_done);
            } else if (k == 0) {
                lea_rip_abs(RAX, K.pre_site);
                mov_mr(1, xm(R14, XO(site)), RAX);
                jmp_a(J.exit_careful0);
            } else {
                jmp_a(J.exit_careful);
            }
        }
        if (k == n)
            break;
        if (K.l_look[k] >= 0) {
            bind(K.l_look[k]);
            emit_add_insns(k + 1);
            mov_rr(1, ARG0, RBX);
            mov_ri(ARG1, b->ops[k].pc);
            mov_ri(ARG2, K.it[k]);
            call_a((const void *)jit_look);
            op_r(0, false, 0x84, RAX, RAX, false); // test al, al
            jcc_a(CC_E, J.exit_notime);
            mov_rm(1, R12, xm(RBX, CO(t)));
            mov_rm(0, RAX, RG(15));
            alu_ri(0, ALU_CMP, RAX, (int32_t)op_next(&b->ops[k]));
            jcc_a(CC_NE, J.lookup);
            // Nothing was taken: carry on with the next instruction, which the block's own count
            // includes.
            alu_mi(1, ALU_SUB, xm(RBX, CO(insns)), k + 1);
            if (K.sb[k + 1])
                alu_rm(1, ALU_SUB, R12, PSK(K.sb[k + 1]));
            emit_budget_check(k + 1);
            jmp_l(k + 1 < n ? K.l_main[k + 1] : K.l_fall);
        }
        if (K.l_dyn[k] >= 0) {
            // A handler sent control elsewhere: block_run's chain point.
            bind(K.l_dyn[k]);
            if (K.sb[k + 1])
                alu_rm(1, ALU_ADD, R12, PSK(K.sb[k + 1]));
            emit_add_insns(k + 1);
            alu_rr(1, ALU_CMP, R12, R13);
            jcc_a(CC_AE, J.exit_done);
            alu_mi(0, ALU_CMP, xm(RBX, CO(itstate)), 0);
            jcc_a(CC_NE, J.exit_done);
            alu_mi(0, ALU_CMP, xm(RBX, CO(state)), CPU_RUN);
            jcc_a(CC_NE, J.exit_done);
            mov_rm(0, RAX, RG(15));
            jmp_a(J.lookup);
        }
    }

    for (int i = 0; i < K.nlinks; i++) {
        uint8_t *stub = J.p;
        mov_mi(RG(15), K.links[i].target);
        lea_rip_abs(RAX, K.links[i].site);
        mov_mr(1, xm(R14, XO(site)), RAX);
        jmp_a(J.exit_link);
        int32_t rel = rel_to(stub, K.links[i].site + 4);
        memcpy(K.links[i].site, &rel, 4);
    }
}

static void *jit_compile(chip_t *m, block_t *b, bool careful)
{
    uint32_t pc0 = b->tag & ~1u;
    if (!b->fixed && pc0 - SRAM_BASE >= 0x90000u)
        return NULL;
    if ((size_t)(J.end - J.p) < JIT_BLOCK_ROOM)
        jit_flush();

    uint8_t *begin = J.p;
    memset(&K, 0, sizeof(K));
    K.b = b;
    K.m = m;
    K.n = b->n;
    K.fixed = b->fixed;
    K.careful = careful;
    memset(K.l_main, -1, sizeof(K.l_main));
    memset(K.l_gen1, -1, sizeof(K.l_gen1));
    memset(K.l_gencyc, -1, sizeof(K.l_gencyc));
    memset(K.l_fetch1, -1, sizeof(K.l_fetch1));
    memset(K.l_fetch2, -1, sizeof(K.l_fetch2));
    memset(K.l_careful, -1, sizeof(K.l_careful));
    memset(K.l_look, -1, sizeof(K.l_look));
    memset(K.l_dyn, -1, sizeof(K.l_dyn));
    memset(K.l_dop, -1, sizeof(K.l_dop));
    memset(K.l_mmio, -1, sizeof(K.l_mmio));
    memset(K.l_skip, -1, sizeof(K.l_skip));
    memset(K.l_post, -1, sizeof(K.l_post));
    memset(K.l_other, -1, sizeof(K.l_other));
    memset(K.l_loaded, -1, sizeof(K.l_loaded));
    nlab = nfix = 0;
    jit_overflow = false;
    for (int k = 0; k <= K.n; k++)
        K.l_main[k] = new_label();
    if (b->nrun < b->n) {
        uint32_t it = b->ops[b->nrun - 1].hw1 & 0xff;
        for (int k = b->nrun; k < K.n; k++) {
            K.it[k] = (uint8_t)it;
            it = (it & 7) ? (it & 0xe0) | ((it << 1) & 0x1f) : 0;
        }
    }
    K.l_fall = new_label();
    K.l_stale = new_label();

    J.p = (uint8_t *)(((uintptr_t)J.p + sizeof(jit_hdr_t) + 15) & ~(uintptr_t)15);
    jit_hdr_t *h = (jit_hdr_t *)(J.p - sizeof(jit_hdr_t));
    h->pc = pc0;
    h->gen = b->gen;
    h->fixed = b->fixed;
    h->bytes = b->bytes;
    memcpy(h->code, b->code, b->bytes);
    uint8_t *entry = J.p;

    emit_block_entry();
    int max_extra = 0;
    for (int k = 0; k < K.n; k++) {
        const dop_t *d = &b->ops[k];
        bind(K.l_main[k]);
        emit_fetch(k, d);
        int extra = 0;
        K.ended = false;
        int st = emit_inline(k, d, &extra);
        if (st < 0) {
            K.generic[k] = true;
            st = 0;
            K.sb[k + 1] = K.sb[k];
            mov_mi(xm(RBX, CO(cyc)), 1);
            bind(lab(&K.l_gencyc[k]));
            emit_generic_body(k, d, 0, false);
        }
        K.sb[k + 1] = K.sb[k] + st;
        if (!K.ended)
            bind(lab(&K.l_post[k]));
        if (K.careful && !K.generic[k] && !K.ended)
            emit_budget_check(k + 1);
        if (k == K.n - 1)
            max_extra = extra;
    }
    bind(K.l_main[K.n]);
    const dop_t *lastd = &b->ops[K.n - 1];
    if (!(!K.generic[K.n - 1] && K.ended)) {
        bind(K.l_fall);
        K.fall_bound = true;
        if (K.sb[K.n])
            alu_rm(1, ALU_ADD, R12, PSK(K.sb[K.n]));
        emit_add_insns(K.n);
        if (K.generic[K.n - 1]) {
            // A handler may have started an IT block or stopped the core.
            alu_mi(0, ALU_CMP, xm(RBX, CO(itstate)), 0);
            jcc_a(CC_NE, J.exit_done);
            alu_mi(0, ALU_CMP, xm(RBX, CO(state)), CPU_RUN);
            jcc_a(CC_NE, J.exit_done);
        }
        emit_link(op_next(lastd));
    }
    K.total = K.sb[K.n] + max_extra;

    emit_cold();
    bind(K.l_stale);
    mov_mi(RG(15), pc0);
    lea_rip_abs(RAX, entry);
    mov_mr(1, xm(R14, XO(site)), RAX);
    jmp_a(J.exit_stale);

    J.p = (uint8_t *)(((uintptr_t)J.p + 7) & ~(uintptr_t)7);
    for (int k = 0; k < K.n; k++)
        if (K.l_dop[k] >= 0) {
            bind(K.l_dop[k]);
            memcpy(J.p, &b->ops[k], sizeof(dop_t));
            J.p += sizeof(dop_t);
        }

    if (jit_overflow || K.total >= JIT_PSK || (size_t)(J.p - begin) > JIT_BLOCK_ROOM) {
        J.p = begin;
        return NULL;
    }
    for (int i = 0; i < K.ntotal; i++) {
        int32_t disp = XO(psk) + 8 * K.total;
        memcpy(K.total_sites[i], &disp, 4);
    }
    for (int i = 0; i < nfix; i++) {
        uint8_t *t = lab_at[fixups[i].label];
        if (!t) {
            J.p = begin;
            return NULL;
        }
        int32_t rel = rel_to(t, fixups[i].site + 4 + fixups[i].trailing);
        memcpy(fixups[i].site, &rel, 4);
    }
    return entry;
}

// ---------------------------------------------------------------------------------------------
// Running

static void *jit_code_for(chip_t *m, block_t *b)
{
    if (b->jit && b->jit_epoch == J.epoch)
        return b->jit;
    uint32_t pc = b->tag & ~1u;
    void *code = jx.lookup[jit_hash(pc)].code;
    if (code && jx.lookup[jit_hash(pc)].pc == pc) {
        // A translation the block cache lost track of serves again if made from the same code.
        const jit_hdr_t *h = (const jit_hdr_t *)((uint8_t *)code - sizeof(jit_hdr_t));
        if (h->fixed == b->fixed &&
            (b->fixed ? h->gen == b->gen : h->bytes == b->bytes && !memcmp(h->code, b->code, b->bytes))) {
            b->jit = code;
            b->jit_epoch = J.epoch;
            return code;
        }
    }
    code = jit_compile(m, b, false);
    if (code) {
        b->jit = code;
        b->jit_epoch = J.epoch;
        jx.lookup[jit_hash(pc)].pc = pc;
        jx.lookup[jit_hash(pc)].code = code;
    }
    return code;
}

static void jit_set_ps(uint32_t ps)
{
    J.ps = ps;
    for (unsigned k = 0; k < JIT_PSK; k++)
        jx.psk[k] = (uint64_t)k * ps;
}

// Like block_run from block b, returning true when it stopped early for block_run to finish the
// time to the exact instruction.
static bool jit_run(cpu_t *c, block_t *b, uint64_t until)
{
    chip_t *m = c->m;
    if (m->sys_ps != J.ps)
        jit_set_ps(m->sys_ps);
    jx.until = until;
    void *code = jit_code_for(m, b);
    if (!code) {
        block_run(c, b, until);
        return false;
    }
    for (;;) {
        int why = J.enter(c, &jx, code);
        if (why == JX_DONE)
            return false;
        if (why == JX_CAREFUL)
            return true;
        if (why == JX_CAREFUL0) {
            // Near the limit: the block's careful translation, made now and kept for next time.
            uint8_t *site = jx.site;
            uint32_t epoch = J.epoch;
            block_t *cb = block_find(m, c->r[15]);
            code = cb ? jit_compile(m, cb, true) : NULL;
            if (!code)
                return true;
            if (epoch == J.epoch) {
                int32_t rel = rel_to(code, site + 4);
                memcpy(site, &rel, 4);
            }
            continue;
        }
        uint8_t *site = jx.site;
        uint32_t epoch = J.epoch, npc = c->r[15];
        block_t *nb = block_find(m, npc);
        if (!nb)
            return false;
        code = jit_code_for(m, nb);
        if (!code) {
            block_run(c, nb, until);
            return false;
        }
        if (epoch == J.epoch) {
            if (why == JX_LINK) {
                int32_t rel = rel_to(code, site + 4);
                memcpy(site, &rel, 4);
            } else if (why == JX_STALE && site != code) {
                site[0] = 0xe9;
                int32_t rel = rel_to(code, site + 5);
                memcpy(site + 1, &rel, 4);
            }
        }
        jx.lookup[jit_hash(npc)].pc = npc;
        jx.lookup[jit_hash(npc)].code = code;
    }
}

#else

static bool jit_available(void)
{
    return false;
}

static void jit_forget(void)
{
}

static bool jit_run(cpu_t *c, block_t *b, uint64_t until)
{
    block_run(c, b, until);
    return false;
}

#endif
