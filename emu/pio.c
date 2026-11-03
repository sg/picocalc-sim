#include <string.h>

#include "chip.h"

// PIO blocks with their state machines executed instruction by instruction at the configured
// clock divider. State machines are brought up to date lazily, when the CPU or DMA touches the
// block, and by a periodic event while any of them is running.

#define PIO_POLL_PS (5 * PS_PER_US)

static void pio_event(chip_t *m, emu_event_t *ev);

void pio_reset(pio_t *p, int idx)
{
    uint64_t owned = p->pin_owned;
    memset(p, 0, sizeof(*p));
    p->pin_owned = owned;
    p->idx = idx;
    for (int i = 0; i < 4; i++) {
        p->sm[i].clkdiv = 1u << 16;
        p->sm[i].execctrl = 0x1f << 12;
        p->sm[i].shiftctrl = 0x000c0000u;
        p->sm[i].pinctrl = 5u << 26;
    }
}

static int fifo_depth_tx(pio_sm_t *sm)
{
    if (sm->shiftctrl & (1u << 30))
        return 8;
    if (sm->shiftctrl & (1u << 31))
        return 0;
    return 4;
}

static int fifo_depth_rx(pio_sm_t *sm)
{
    if (sm->shiftctrl & (1u << 31))
        return 8;
    if (sm->shiftctrl & (1u << 30))
        return 0;
    return 4;
}

static void update_irqs(chip_t *m, pio_t *p)
{
    uint32_t intr = p->irq & 0xff;
    uint32_t raw = (intr & 0xf) << 8;
    for (int i = 0; i < 4; i++) {
        pio_sm_t *sm = &p->sm[i];
        if (sm->rxn > 0)
            raw |= 1u << i;
        if (sm->txn < fifo_depth_tx(sm))
            raw |= 1u << (4 + i);
    }
    (void)intr;
    // Every PIO access ends here, and the lines rarely change; setting a line to the level it
    // already has does nothing, so only changes are passed on, in the same order as before.
    for (int k = 0; k < 2; k++) {
        unsigned irq = IRQ_PIO0_0 + 2u * (unsigned)p->idx + (unsigned)k;
        bool level = ((raw & p->inte[k]) | p->intf[k]) != 0;
        if (((m->irq_lines >> irq) & 1) != level)
            chip_irq(m, irq, level);
    }
    for (int i = 0; i < 4; i++) {
        pio_sm_t *sm = &p->sm[i];
        unsigned tx = DREQ_PIO0_TX0 + 8u * (unsigned)p->idx + (unsigned)i;
        unsigned rx = DREQ_PIO0_RX0 + 8u * (unsigned)p->idx + (unsigned)i;
        bool tx_level = sm->txn < fifo_depth_tx(sm);
        if (m->dreq[tx] != tx_level)
            chip_dreq(m, tx, tx_level);
        bool rx_level = sm->rxn > 0;
        if (m->dreq[rx] != rx_level)
            chip_dreq(m, rx, rx_level);
    }
}

static unsigned base_pin(pio_t *p, unsigned rel)
{
    return (rel + ((p->gpiobase & 0x10) ? 16 : 0)) & 63;
}

static void write_pins(chip_t *m, pio_t *p, unsigned base, unsigned count, uint32_t val, bool dirs)
{
    uint64_t *t = dirs ? &p->out_en : &p->out_val;
    uint64_t old = *t;
    uint64_t nv = old;
    for (unsigned i = 0; i < count; i++) {
        unsigned pin = base_pin(p, (base + i) & 31);
        uint64_t bit = 1ull << pin;
        nv = ((val >> i) & 1) ? (nv | bit) : (nv & ~bit);
    }
    if (nv == old)
        return;
    *t = nv;
    uint64_t diff = (nv ^ old) & p->pin_owned;
    if (diff)
        gpio_refresh_mask(m, diff);
}

static uint32_t read_pins(chip_t *m, pio_t *p, unsigned base)
{
    uint64_t all = gpio_in_all(m);
    uint32_t v = 0;
    for (unsigned i = 0; i < 32; i++) {
        unsigned pin = base_pin(p, (base + i) & 31);
        if ((all >> pin) & 1)
            v |= 1u << i;
    }
    return v;
}

bool pio_drives(chip_t *m, int idx, unsigned pin, bool *level)
{
    pio_t *p = &m->pio[idx];
    *level = (p->out_val >> pin) & 1;
    return (p->out_en >> pin) & 1;
}

static uint32_t bitrev(uint32_t x)
{
    x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
    x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
    x = ((x >> 4) & 0x0f0f0f0fu) | ((x & 0x0f0f0f0fu) << 4);
    return __builtin_bswap32(x);
}

static bool tx_pop(pio_sm_t *sm, uint32_t *v)
{
    if (!sm->txn)
        return false;
    *v = sm->txf[sm->txh];
    sm->txh = (sm->txh + 1) & 7;
    sm->txn--;
    return true;
}

static bool rx_push(pio_sm_t *sm, uint32_t v)
{
    if (sm->rxn >= fifo_depth_rx(sm))
        return false;
    sm->rxf[(sm->rxh + sm->rxn) & 7] = v;
    sm->rxn++;
    return true;
}

static unsigned irq_index(pio_t *p, int smi, unsigned field, int *target_pio)
{
    unsigned mode = (field >> 3) & 3;
    unsigned idx = field & 7;
    *target_pio = p->idx;
    if (mode == 2)
        idx = (idx & 4) | ((idx + (unsigned)smi) & 3);
    else if (mode == 1)
        *target_pio = (p->idx + 2) % 3;
    else if (mode == 3)
        *target_pio = (p->idx + 1) % 3;
    return idx;
}

typedef enum { EX_DONE, EX_STALL } ex_t;

static void sm_restart(pio_sm_t *sm)
{
    sm->isr = 0;
    sm->osr = 0;
    sm->isr_count = 0;
    sm->osr_count = 32;
    sm->delay = 0;
    sm->stalled = false;
    sm->push_stall = false;
}

static ex_t execute(chip_t *m, pio_t *p, int smi, uint16_t ins, bool *jumped)
{
    pio_sm_t *sm = &p->sm[smi];
    uint32_t pinctrl = sm->pinctrl, shiftctrl = sm->shiftctrl, execctrl = sm->execctrl;
    unsigned op = ins >> 13;
    unsigned arg1 = (ins >> 5) & 7, arg2 = ins & 0x1f;
    unsigned pull_thresh = ((shiftctrl >> 25) & 31) ? ((shiftctrl >> 25) & 31) : 32;
    unsigned push_thresh = ((shiftctrl >> 20) & 31) ? ((shiftctrl >> 20) & 31) : 32;
    bool out_right = shiftctrl & (1u << 19), in_right = shiftctrl & (1u << 18);
    bool autopull = shiftctrl & (1u << 17), autopush = shiftctrl & (1u << 16);
    *jumped = false;

    switch (op) {
    case 0: {
        bool take;
        switch (arg1) {
        case 0: take = true; break;
        case 1: take = sm->x == 0; break;
        case 2: take = sm->x != 0; sm->x--; break;
        case 3: take = sm->y == 0; break;
        case 4: take = sm->y != 0; sm->y--; break;
        case 5: take = sm->x != sm->y; break;
        case 6: take = (read_pins(m, p, (execctrl >> 24) & 31) & 1) != 0; break;
        default: take = sm->osr_count < (int)pull_thresh; break;
        }
        if (take) {
            sm->pc = arg2;
            *jumped = true;
        }
        return EX_DONE;
    }
    case 1: {
        bool pol = ins & 0x80;
        unsigned src = (ins >> 5) & 3;
        bool v;
        if (src == 0) {
            v = gpio_in(m, base_pin(p, arg2));
        } else if (src == 1) {
            v = (read_pins(m, p, ((pinctrl >> 15) & 31) + arg2) & 1) != 0;
        } else if (src == 2) {
            int tp;
            unsigned idx = irq_index(p, smi, arg2, &tp);
            pio_t *t = &m->pio[tp];
            v = (t->irq >> idx) & 1;
            if (v && pol) {
                t->irq &= ~(1u << idx);
                return EX_DONE;
            }
        } else {
            v = (read_pins(m, p, ((execctrl >> 24) & 31) + (arg2 & 3)) & 1) != 0;
        }
        return v == pol ? EX_DONE : EX_STALL;
    }
    case 2: {
        unsigned n = arg2 ? arg2 : 32;
        if (sm->push_stall) {
            if (!rx_push(sm, sm->isr)) {
                p->fdebug |= 1u << smi;
                return EX_STALL;
            }
            sm->isr = 0;
            sm->isr_count = 0;
            sm->push_stall = false;
            return EX_DONE;
        }
        uint32_t data;
        switch (arg1) {
        case 0: data = read_pins(m, p, (pinctrl >> 15) & 31); break;
        case 1: data = sm->x; break;
        case 2: data = sm->y; break;
        case 6: data = sm->isr; break;
        case 7: data = sm->osr; break;
        default: data = 0; break;
        }
        if (n < 32)
            data &= (1u << n) - 1;
        if (in_right)
            sm->isr = n == 32 ? data : (sm->isr >> n) | (data << (32 - n));
        else
            sm->isr = n == 32 ? data : (sm->isr << n) | data;
        sm->isr_count += (int)n;
        if (sm->isr_count > 32)
            sm->isr_count = 32;
        if (autopush && sm->isr_count >= (int)push_thresh) {
            if (!rx_push(sm, sm->isr)) {
                sm->push_stall = true;
                p->fdebug |= 1u << smi;
                return EX_STALL;
            }
            sm->isr = 0;
            sm->isr_count = 0;
        }
        return EX_DONE;
    }
    case 3: {
        unsigned n = arg2 ? arg2 : 32;
        if (autopull && sm->osr_count >= (int)pull_thresh) {
            uint32_t v;
            if (!tx_pop(sm, &v)) {
                p->fdebug |= 1u << (24 + smi);
                return EX_STALL;
            }
            sm->osr = v;
            sm->osr_count = 0;
        }
        uint32_t data;
        if (out_right) {
            data = n == 32 ? sm->osr : sm->osr & ((1u << n) - 1);
            sm->osr = n == 32 ? 0 : sm->osr >> n;
        } else {
            data = n == 32 ? sm->osr : sm->osr >> (32 - n);
            sm->osr = n == 32 ? 0 : sm->osr << n;
        }
        sm->osr_count += (int)n;
        if (sm->osr_count > 32)
            sm->osr_count = 32;
        switch (arg1) {
        case 0: write_pins(m, p, pinctrl & 31, (pinctrl >> 20) & 63, data, false); break;
        case 1: sm->x = data; break;
        case 2: sm->y = data; break;
        case 4: write_pins(m, p, pinctrl & 31, (pinctrl >> 20) & 63, data, true); break;
        case 5:
            sm->pc = data & 31;
            *jumped = true;
            break;
        case 6:
            sm->isr = data;
            sm->isr_count = (int)n;
            break;
        case 7: {
            bool j;
            execute(m, p, smi, (uint16_t)data, &j);
            *jumped = true;
            if (!j)
                sm->pc = sm->pc;
            break;
        }
        default: break;
        }
        if (autopull && sm->osr_count >= (int)pull_thresh) {
            uint32_t v;
            if (tx_pop(sm, &v)) {
                sm->osr = v;
                sm->osr_count = 0;
            }
        }
        return EX_DONE;
    }
    case 4: {
        bool pull = ins & 0x80, ifx = ins & 0x40, block = ins & 0x20;
        if (ins & 0x10) {
            unsigned idx = ins & 3;
            if (pull)
                sm->osr = sm->rxf[idx & 3];
            else
                sm->rxf[idx & 3] = sm->isr;
            return EX_DONE;
        }
        if (!pull) {
            if (ifx && sm->isr_count < (int)push_thresh)
                return EX_DONE;
            if (!rx_push(sm, sm->isr)) {
                if (block) {
                    p->fdebug |= 1u << smi;
                    return EX_STALL;
                }
                return EX_DONE;
            }
            sm->isr = 0;
            sm->isr_count = 0;
            return EX_DONE;
        }
        if (ifx && sm->osr_count < (int)pull_thresh)
            return EX_DONE;
        uint32_t v;
        if (!tx_pop(sm, &v)) {
            if (block) {
                p->fdebug |= 1u << (24 + smi);
                return EX_STALL;
            }
            sm->osr = sm->x;
            sm->osr_count = 0;
            return EX_DONE;
        }
        sm->osr = v;
        sm->osr_count = 0;
        return EX_DONE;
    }
    case 5: {
        unsigned dst = arg1, mop = (ins >> 3) & 3, src = ins & 7;
        uint32_t v;
        switch (src) {
        case 0: v = read_pins(m, p, (pinctrl >> 15) & 31); break;
        case 1: v = sm->x; break;
        case 2: v = sm->y; break;
        case 5: {
            unsigned sel = (execctrl >> 5) & 3, n = execctrl & 31;
            if (sel == 0)
                v = sm->txn < (int)n ? 0xffffffffu : 0;
            else if (sel == 1)
                v = sm->rxn < (int)n ? 0xffffffffu : 0;
            else {
                int tp;
                unsigned idx = irq_index(p, smi, n & 0x1f, &tp);
                v = ((m->pio[tp].irq >> idx) & 1) ? 0xffffffffu : 0;
            }
            break;
        }
        case 6: v = sm->isr; break;
        case 7: v = sm->osr; break;
        default: v = 0; break;
        }
        if (mop == 1)
            v = ~v;
        else if (mop == 2)
            v = bitrev(v);
        switch (dst) {
        case 0: write_pins(m, p, pinctrl & 31, (pinctrl >> 20) & 63, v, false); break;
        case 1: sm->x = v; break;
        case 2: sm->y = v; break;
        case 3: write_pins(m, p, pinctrl & 31, (pinctrl >> 20) & 63, v, true); break;
        case 4: {
            bool j;
            execute(m, p, smi, (uint16_t)v, &j);
            *jumped = true;
            break;
        }
        case 5:
            sm->pc = v & 31;
            *jumped = true;
            break;
        case 6:
            sm->isr = v;
            sm->isr_count = 0;
            break;
        case 7:
            sm->osr = v;
            sm->osr_count = 0;
            break;
        default: break;
        }
        return EX_DONE;
    }
    case 6: {
        bool clr = ins & 0x40, wait = ins & 0x20;
        int tp;
        unsigned idx = irq_index(p, smi, arg2, &tp);
        pio_t *t = &m->pio[tp];
        if (clr) {
            t->irq &= ~(1u << idx);
            return EX_DONE;
        }
        if (!sm->stalled)
            t->irq |= 1u << idx;
        if (wait && (t->irq >> idx) & 1)
            return EX_STALL;
        return EX_DONE;
    }
    default: {
        switch (arg1) {
        case 0: write_pins(m, p, (pinctrl >> 5) & 31, (pinctrl >> 26) & 7, arg2, false); break;
        case 1: sm->x = arg2; break;
        case 2: sm->y = arg2; break;
        case 4: write_pins(m, p, (pinctrl >> 5) & 31, (pinctrl >> 26) & 7, arg2, true); break;
        default: break;
        }
        return EX_DONE;
    }
    }
}

static void side_set(chip_t *m, pio_t *p, pio_sm_t *sm, uint16_t ins)
{
    unsigned count = (sm->pinctrl >> 29) & 7;
    if (!count)
        return;
    bool opt = sm->execctrl & (1u << 30);
    unsigned field = (ins >> 8) & 0x1f;
    unsigned bits = field >> (5 - count);
    if (opt) {
        if (!(bits >> (count - 1)))
            return;
        count--;
        bits &= (1u << count) - 1;
    }
    if (!count)
        return;
    bool pindir = sm->execctrl & (1u << 29);
    write_pins(m, p, (sm->pinctrl >> 10) & 31, count, bits, pindir);
}

static unsigned delay_bits(pio_sm_t *sm, uint16_t ins)
{
    unsigned count = (sm->pinctrl >> 29) & 7;
    unsigned field = (ins >> 8) & 0x1f;
    unsigned dbits = 5 - count;
    return field & ((1u << dbits) - 1);
}

static void sm_step(chip_t *m, pio_t *p, int smi)
{
    pio_sm_t *sm = &p->sm[smi];
    if (sm->delay > 0) {
        sm->delay--;
        return;
    }
    uint16_t ins = p->instr[sm->pc & 31];
    if (!sm->stalled)
        side_set(m, p, sm, ins);
    bool jumped;
    ex_t r = execute(m, p, smi, ins, &jumped);
    if (r == EX_STALL) {
        sm->stalled = true;
        return;
    }
    sm->stalled = false;
    if (!jumped) {
        unsigned top = (sm->execctrl >> 12) & 31, bottom = (sm->execctrl >> 7) & 31;
        sm->pc = sm->pc == top ? bottom : (sm->pc + 1) & 31;
    }
    sm->delay = (int)delay_bits(sm, ins);
}


// A state machine stalled on a FIFO only the CPU or DMA can service does nothing until they do,
// though the hardware keeps reasserting its stall flag every cycle.
static bool sm_blocked_on_cpu(pio_t *p, int smi)
{
    pio_sm_t *sm = &p->sm[smi];
    if (!sm->stalled || sm->delay)
        return false;
    uint16_t ins = p->instr[sm->pc & 31];
    unsigned op = ins >> 13;
    bool blocked = false;
    bool tx = true;
    if (op == 4 && (ins & 0x80) && !(ins & 0x10))
        blocked = sm->txn == 0;
    else if (op == 4 && !(ins & 0x10)) {
        blocked = sm->rxn >= fifo_depth_rx(sm);
        tx = false;
    } else if (op == 3)
        blocked = sm->txn == 0;
    else if (op == 2) {
        blocked = sm->push_stall && sm->rxn >= fifo_depth_rx(sm);
        tx = false;
    }
    if (blocked)
        p->fdebug |= tx ? 1u << (24 + smi) : 1u << smi;
    return blocked;
}

static uint64_t sm_period(chip_t *m, pio_sm_t *sm)
{
    uint32_t i = sm->clkdiv >> 16, f = (sm->clkdiv >> 8) & 0xff;
    if (!i)
        i = 65536;
    return (uint64_t)m->sys_ps * (i * 256 + f) / 256;
}

// "out pins, 1 side 0" followed by "nop side 1", wrapping, with autopull: the serial transmitter
// PicoCalc LCD drivers use. Recognised programs run a bit at a time straight into the device on
// those pins instead of being stepped instruction by instruction.
static bool spi_out_program(pio_t *p, pio_sm_t *sm, unsigned *sck, unsigned *mosi)
{
    unsigned top = (sm->execctrl >> 12) & 31, bottom = (sm->execctrl >> 7) & 31;
    if (top != bottom + 1 || !(sm->shiftctrl & (1u << 17)))
        return false;
    if (((sm->pinctrl >> 29) & 7) != 1 || (sm->execctrl & (3u << 29)) || ((sm->pinctrl >> 20) & 63) != 1)
        return false;
    if (p->instr[bottom] != 0x6001 || p->instr[top] != 0xb042)
        return false;
    *sck = base_pin(p, (sm->pinctrl >> 10) & 31);
    *mosi = base_pin(p, sm->pinctrl & 31);
    return true;
}

static void set_pin_quiet(chip_t *m, pio_t *p, unsigned pin, bool v)
{
    uint64_t bit = 1ull << pin;
    p->out_val = v ? (p->out_val | bit) : (p->out_val & ~bit);
    if ((p->pin_owned & bit) && pin < NUM_GPIO_MAX) {
        m->gpio[pin].level = v;
        m->gpio_in_valid = false;
    }
}

static bool run_spi_out(chip_t *m, pio_t *p, int smi)
{
    pio_sm_t *sm = &p->sm[smi];
    unsigned sck, mosi;
    if (sm->delay || sm->pc != ((sm->execctrl >> 7) & 31) || !spi_out_program(p, sm, &sck, &mosi))
        return false;
    uint64_t period = sm_period(m, sm);
    unsigned thresh = ((sm->shiftctrl >> 25) & 31) ? ((sm->shiftctrl >> 25) & 31) : 32;
    bool right = sm->shiftctrl & (1u << 19);
    int last = -1;
    while (sm->next_t <= m->now) {
        if (sm->osr_count >= (int)thresh) {
            uint32_t v;
            if (!tx_pop(sm, &v)) {
                p->fdebug |= 1u << (24 + smi);
                sm->stalled = true;
                set_pin_quiet(m, p, sck, false);
                sm->next_t = m->now + 1;
                break;
            }
            sm->osr = v;
            sm->osr_count = 0;
        }
        sm->stalled = false;
        int bit = right ? (int)(sm->osr & 1) : (int)(sm->osr >> 31);
        sm->osr = right ? sm->osr >> 1 : sm->osr << 1;
        sm->osr_count++;
        devices_spi_bit(m, sck, mosi, bit);
        last = bit;
        sm->next_t += 2 * period;
    }
    if (last >= 0) {
        set_pin_quiet(m, p, mosi, last);
        set_pin_quiet(m, p, sck, true);
    }
    return true;
}

// "L: jmp x!=y L+2 / L+2: nop / L+3: jmp y-- L", the count of the PWM program in pico-examples
// that PIO audio drivers use: until Y reaches X or zero, each pass only decrements Y, which nothing
// else can see, so passes are skipped in bulk up to the present.
static bool skip_count_loop(chip_t *m, pio_t *p, int smi)
{
    pio_sm_t *sm = &p->sm[smi];
    unsigned l = sm->pc & 31, top = (sm->execctrl >> 12) & 31;
    if (sm->delay || sm->stalled || l > 28 || top == l + 2)
        return false;
    // Zero side-set and delay fields do nothing only when side-set is optional or absent.
    if (((sm->pinctrl >> 29) & 7) && !(sm->execctrl & (1u << 30)))
        return false;
    if (p->instr[l] != (0x00a0 | (l + 2)) || p->instr[l + 2] != 0xa042 || p->instr[l + 3] != (0x0080 | l))
        return false;
    uint32_t x = sm->x, y = sm->y, k = x < y ? y - x : x > y ? y : 0;
    uint64_t period = sm_period(m, sm);
    uint64_t fit = ((m->now - sm->next_t) / period + 1) / 3;
    if (fit < k)
        k = (uint32_t)fit;
    if (!k)
        return false;
    sm->y -= k;
    sm->next_t += 3 * (uint64_t)k * period;
    return true;
}

// The cyw43 driver's half-duplex bus: "out pins,1 / jmp x--" shifts the command and any data out,
// "set pindirs,0" (and a nop or two) turns the data pin round, and "in pins,1 / jmp y--" shifts
// the reply in, with the clock on side-set. Recognised programs trade bits with the radio model
// directly instead of through clock edges on the pins.
typedef struct {
    unsigned out_pc, set_pc, in_pc, end_pc, clk, data;
} gspi_prog_t;

static bool gspi_program(chip_t *m, pio_t *p, pio_sm_t *sm, gspi_prog_t *g)
{
    if (!m->wifi)
        return false;
    uint32_t pinctrl = sm->pinctrl;
    if (((pinctrl >> 29) & 7) != 1 || (sm->execctrl & (3u << 29)) || (sm->shiftctrl & 0xffff0000u) != 0x00030000u)
        return false;
    unsigned outb = pinctrl & 31, setb = (pinctrl >> 5) & 31, inb = (pinctrl >> 15) & 31;
    if (((pinctrl >> 20) & 63) != 1 || ((pinctrl >> 26) & 7) != 1 || setb != outb || inb != outb)
        return false;
    unsigned b = (sm->execctrl >> 7) & 31;
    if (p->instr[b] != 0x6001 || p->instr[(b + 1) & 31] != (0x1040 | b))
        return false;
    unsigned k = (b + 2) & 31;
    if (p->instr[k] != 0xe080)
        return false;
    g->set_pc = k;
    k = (k + 1) & 31;
    for (int n = 0; n < 3 && (p->instr[k] == 0xa042 || p->instr[k] == 0xb042); n++)
        k = (k + 1) & 31;
    uint16_t in = p->instr[k];
    if ((in & 0xefff) != 0x4001 || p->instr[(k + 1) & 31] != ((0x0080u | k) | ((in & 0x1000u) ^ 0x1000u)))
        return false;
    g->out_pc = b;
    g->in_pc = k;
    g->end_pc = (k + 1) & 31;
    g->clk = base_pin(p, (pinctrl >> 10) & 31);
    g->data = base_pin(p, outb);
    return cyw43_bus_pins(m, g->clk, g->data);
}

static bool run_gspi(chip_t *m, pio_t *p, int smi)
{
    pio_sm_t *sm = &p->sm[smi];
    gspi_prog_t g;
    if (sm->delay || !gspi_program(m, p, sm, &g))
        return false;
    unsigned pc = sm->pc;
    if (((pc - g.out_pc) & 31) > ((g.end_pc - g.out_pc) & 31))
        return false;
    unsigned top = (sm->execctrl >> 12) & 31;
    uint64_t period = sm_period(m, sm);
    while (sm->next_t <= m->now) {
        uint16_t ins = p->instr[pc];
        unsigned next = pc == top ? g.out_pc : (pc + 1) & 31;
        set_pin_quiet(m, p, g.clk, (ins & 0x1000) != 0);
        if (pc == g.out_pc) {
            if (sm->osr_count >= 32) {
                uint32_t v;
                if (!tx_pop(sm, &v)) {
                    p->fdebug |= 1u << (24 + smi);
                    sm->stalled = true;
                    sm->next_t = m->now + 1;
                    break;
                }
                sm->osr = v;
                sm->osr_count = 0;
            }
            int bit = (int)(sm->osr >> 31);
            sm->osr <<= 1;
            sm->osr_count++;
            set_pin_quiet(m, p, g.data, bit);
            cyw43_bus_out(m, bit);
            pc = next;
        } else if (pc == ((g.out_pc + 1) & 31)) {
            bool take = sm->x != 0;
            sm->x--;
            pc = take ? g.out_pc : next;
        } else if (pc == g.set_pc) {
            write_pins(m, p, (sm->pinctrl >> 5) & 31, 1, 0, true);
            pc = next;
        } else if (pc == g.in_pc) {
            if (!sm->push_stall) {
                sm->isr = (sm->isr << 1) | (uint32_t)cyw43_bus_in(m);
                sm->isr_count++;
                sm->push_stall = sm->isr_count >= 32;
            }
            if (sm->push_stall) {
                if (!rx_push(sm, sm->isr)) {
                    p->fdebug |= 1u << smi;
                    sm->stalled = true;
                    sm->next_t = m->now + 1;
                    break;
                }
                sm->isr = 0;
                sm->isr_count = 0;
                sm->push_stall = false;
            }
            pc = next;
        } else if (pc == g.end_pc) {
            bool take = sm->y != 0;
            sm->y--;
            pc = take ? g.in_pc : next;
        } else {
            pc = next;
        }
        sm->stalled = false;
        sm->next_t += period;
    }
    sm->pc = pc;
    return true;
}

void pio_sync(chip_t *m, pio_t *p)
{
    uint32_t en = p->ctrl & 0xf;
    if (!en) {
        p->t = m->now;
        return;
    }
    while (true) {
        uint64_t next = UINT64_MAX;
        int which = -1;
        for (int i = 0; i < 4; i++) {
            if (!(en & (1u << i)))
                continue;
            if (p->sm[i].next_t < next) {
                next = p->sm[i].next_t;
                which = i;
            }
        }
        if (which < 0 || next > m->now)
            break;
        pio_sm_t *sm = &p->sm[which];
        if (run_gspi(m, p, which) || run_spi_out(m, p, which) || skip_count_loop(m, p, which))
            continue;
        if (sm_blocked_on_cpu(p, which)) {
            sm->next_t = m->now + 1;
            continue;
        }
        uint64_t period = sm_period(m, sm);
        if (sm->delay > 1) {
            uint64_t skip = (m->now - sm->next_t) / period;
            if (skip > (uint64_t)(sm->delay - 1))
                skip = (uint64_t)(sm->delay - 1);
            sm->delay -= (int)skip;
            sm->next_t += skip * period;
        }
        uint64_t edge_t = m->edge_t;
        m->edge_t = sm->next_t;
        sm_step(m, p, which);
        m->edge_t = edge_t;
        sm->next_t += period;
    }
    p->t = m->now;
    update_irqs(m, p);
    ev_cancel(m, &p->ev);
    p->ev.fn = pio_event;
    p->ev.ctx = p;
    ev_schedule(m, &p->ev, m->now + PIO_POLL_PS);
}

static void pio_event(chip_t *m, emu_event_t *ev)
{
    pio_sync(m, ev->ctx);
}

uint32_t pio_read(chip_t *m, pio_t *p, uint32_t off, bool peek)
{
    if (!peek)
        pio_sync(m, p);
    switch (off) {
    case 0x000: return p->ctrl & 0xf;
    case 0x004: {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            pio_sm_t *sm = &p->sm[i];
            if (sm->rxn >= fifo_depth_rx(sm))
                v |= 1u << i;
            if (sm->rxn == 0)
                v |= 1u << (8 + i);
            if (sm->txn >= fifo_depth_tx(sm))
                v |= 1u << (16 + i);
            if (sm->txn == 0)
                v |= 1u << (24 + i);
        }
        return v;
    }
    case 0x008: return p->fdebug;
    case 0x00c: {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++)
            v |= ((uint32_t)p->sm[i].txn | ((uint32_t)p->sm[i].rxn << 4)) << (8 * i);
        return v;
    }
    case 0x020:
    case 0x024:
    case 0x028:
    case 0x02c: {
        pio_sm_t *sm = &p->sm[(off - 0x20) >> 2];
        if (!sm->rxn) {
            if (!peek)
                p->fdebug |= 1u << (8 + ((off - 0x20) >> 2));
            return 0;
        }
        uint32_t v = sm->rxf[sm->rxh];
        if (!peek) {
            sm->rxh = (sm->rxh + 1) & 7;
            sm->rxn--;
            update_irqs(m, p);
        }
        return v;
    }
    case 0x030: return p->irq & 0xff;
    case 0x038: return p->input_sync_bypass;
    case 0x03c: return (uint32_t)p->out_val;
    case 0x040: return (uint32_t)p->out_en;
    case 0x044: return 0x00100404u | (2u << 28);
    case 0x168: return p->gpiobase;
    case 0x16c:
    case 0x178:
    case 0x184: {
        uint32_t raw = (p->irq & 0xf) << 8;
        for (int i = 0; i < 4; i++) {
            if (p->sm[i].rxn > 0)
                raw |= 1u << i;
            if (p->sm[i].txn < fifo_depth_tx(&p->sm[i]))
                raw |= 1u << (4 + i);
        }
        if (off == 0x16c)
            return raw;
        int k = off == 0x178 ? 0 : 1;
        return (raw & p->inte[k]) | p->intf[k];
    }
    case 0x170: return p->inte[0];
    case 0x174: return p->intf[0];
    case 0x17c: return p->inte[1];
    case 0x180: return p->intf[1];
    default:
        if (off >= 0x048 && off < 0x0c8)
            return 0;
        if (off >= 0x0c8 && off < 0x128) {
            pio_sm_t *sm = &p->sm[(off - 0xc8) / 0x18];
            switch ((off - 0xc8) % 0x18) {
            case 0x00: return sm->clkdiv;
            case 0x04: return sm->execctrl | (sm->stalled ? (1u << 31) : 0);
            case 0x08: return sm->shiftctrl;
            case 0x0c: return sm->pc;
            case 0x10: return p->instr[sm->pc & 31];
            default: return sm->pinctrl;
            }
        }
        if (off >= 0x128 && off < 0x168) {
            pio_sm_t *sm = &p->sm[(off - 0x128) >> 4];
            return sm->rxf[((off - 0x128) >> 2) & 3];
        }
        return 0;
    }
}

void pio_write(chip_t *m, pio_t *p, uint32_t off, uint32_t v)
{
    pio_sync(m, p);
    switch (off) {
    case 0x000: {
        uint32_t old = p->ctrl & 0xf;
        for (int i = 0; i < 4; i++) {
            if (v & (1u << (4 + i)))
                sm_restart(&p->sm[i]);
            if (v & (1u << (8 + i)))
                p->sm[i].div_acc = 0;
            if ((v & (1u << i)) && !(old & (1u << i)))
                p->sm[i].next_t = m->now;
        }
        p->ctrl = v & 0xf;
        break;
    }
    case 0x008: p->fdebug &= ~v; break;
    case 0x010:
    case 0x014:
    case 0x018:
    case 0x01c: {
        pio_sm_t *sm = &p->sm[(off - 0x10) >> 2];
        if (sm->txn < fifo_depth_tx(sm)) {
            sm->txf[(sm->txh + sm->txn) & 7] = v;
            sm->txn++;
        } else {
            p->fdebug |= 1u << (16 + ((off - 0x10) >> 2));
        }
        break;
    }
    case 0x030: p->irq &= ~(v & 0xff); break;
    case 0x034: p->irq |= v & 0xff; break;
    case 0x038: p->input_sync_bypass = v; break;
    case 0x168: p->gpiobase = v & 0x10; break;
    case 0x170: p->inte[0] = v & 0xfff; break;
    case 0x174: p->intf[0] = v & 0xfff; break;
    case 0x17c: p->inte[1] = v & 0xfff; break;
    case 0x180: p->intf[1] = v & 0xfff; break;
    default:
        if (off >= 0x048 && off < 0x0c8) {
            p->instr[(off - 0x48) >> 2] = (uint16_t)v;
        } else if (off >= 0x0c8 && off < 0x128) {
            int smi = (int)((off - 0xc8) / 0x18);
            pio_sm_t *sm = &p->sm[smi];
            switch ((off - 0xc8) % 0x18) {
            case 0x00: sm->clkdiv = v & 0xffffff00u; break;
            case 0x04: sm->execctrl = v & 0x7fffffffu; break;
            case 0x08:
                if ((v ^ sm->shiftctrl) & 0xc0000000u) {
                    sm->txn = sm->rxn = 0;
                    sm->txh = sm->rxh = 0;
                }
                sm->shiftctrl = v;
                break;
            case 0x10: {
                bool jumped;
                ex_t r = execute(m, p, smi, (uint16_t)v, &jumped);
                if (r == EX_DONE)
                    sm->stalled = false;
                break;
            }
            case 0x14: sm->pinctrl = v; break;
            default: break;
            }
        } else if (off >= 0x128 && off < 0x168) {
            pio_sm_t *sm = &p->sm[(off - 0x128) >> 4];
            sm->rxf[((off - 0x128) >> 2) & 3] = v;
        }
        break;
    }
    update_irqs(m, p);
    pio_sync(m, p);
}
