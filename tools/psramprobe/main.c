#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/structs/sysinfo.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"

#include "drivers/fat32.h"
#include "drivers/picocalc.h"

#include "probe.pio.h"
#include "psram_spi.pio.h"

// Measures how the PicoCalc mainboard's PSRAM (ESP-PSRAM64H on GP2/GP3/GP20/GP21) answers a PIO
// program on real hardware, and how rp2040-psram's two read programs fare, for the emulator to
// match. The results go to the screen and to PSPROBE.TXT on the SD card.

#define VERSION "1"

#define PIN_MOSI 2
#define PIN_MISO 3
#define PIN_CS 20
#define PIN_SCK 21

#define CAP_WORDS 64
#define CAP_CYCLES (CAP_WORDS * 32)
#define CAPTURES 32

#define ADDR_ALT 0x000100u
#define ADDR_RAND 0x001000u
#define ADDR_WRITE 0x002000u
#define RAND_LEN 64

#define REPORT_FILE "/PSPROBE.TXT"

static PIO const pio = pio1;
enum { SM_DRV, SM_MISO, SM_SCK, SM_LIB };

static uint off_drv, off_samp, off_fudge, off_plain;
static bool drv_loaded;
static uint16_t drv_code[32];
static int ch_miso, ch_sck, ch_tx, ch_rx;
static uint32_t cap_miso[CAP_WORDS], cap_sck[CAP_WORDS];
// Cycles in each phase of the probe's SPI clock.
static unsigned phase;

static enum vreg_voltage base_vreg;
static uint8_t rand_data[RAND_LEN];
static uint32_t rng = 0x2545f491u;

// Set by the keyboard driver on Ctrl+C.
volatile bool user_interrupt;

static char report[32768];
static size_t report_len;

static void vadd(bool screen, const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    if (report_len < sizeof(report) - 1) {
        int n = vsnprintf(report + report_len, sizeof(report) - report_len, fmt, ap);
        if (n > 0)
            report_len += (size_t)n;
        if (report_len > sizeof(report) - 1)
            report_len = sizeof(report) - 1;
    }
    if (screen)
        vprintf(fmt, copy);
    va_end(copy);
}

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vadd(true, fmt, ap);
    va_end(ap);
}

static void note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vadd(false, fmt, ap);
    va_end(ap);
}

static bool save_report(void)
{
    fat32_file_t f;
    fat32_delete(REPORT_FILE);
    if (fat32_create(&f, REPORT_FILE) != FAT32_OK)
        return false;
    size_t done = 0;
    fat32_error_t e = fat32_write(&f, report, report_len, &done);
    fat32_close(&f);
    return e == FAT32_OK && done == report_len;
}

static uint8_t rnd8(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (uint8_t)(rng >> 8);
}

// The regulator goes up before the clock does and comes back down after it.
static bool set_clock(uint32_t khz)
{
    uint vco, p1, p2;
    if (!check_sys_clock_khz(khz, &vco, &p1, &p2))
        return false;
    if (khz > 200000 && vreg_get_voltage() < VREG_VOLTAGE_1_20) {
        vreg_set_voltage(VREG_VOLTAGE_1_20);
        busy_wait_us(1000);
    }
    set_sys_clock_pll(vco, p1, p2);
    if (khz <= 200000 && vreg_get_voltage() != base_vreg)
        vreg_set_voltage(base_vreg >= VREG_VOLTAGE_1_05 && base_vreg <= VREG_VOLTAGE_1_20 ? base_vreg : VREG_VOLTAGE_DEFAULT);
    return true;
}

static unsigned vreg_mv(enum vreg_voltage v)
{
    return v <= VREG_VOLTAGE_1_30 ? 550 + 50 * (unsigned)v : 0;
}

static void load_driver(unsigned h)
{
    pio_program_t prog = {.instructions = drv_code, .length = probe_spi_program.length, .origin = -1};
    for (unsigned i = 0; i < probe_spi_program.length; i++) {
        uint16_t ins = probe_spi_program_instructions[i];
        if ((ins >> 8) & 7)
            ins = (uint16_t)((ins & ~0x0700u) | ((h - 1) << 8));
        drv_code[i] = ins;
    }
    if (drv_loaded) {
        pio_remove_program(pio, &prog, off_drv);
        pio_add_program_at_offset(pio, &prog, off_drv);
    } else {
        off_drv = (uint)pio_add_program(pio, &prog);
        drv_loaded = true;
    }
    pio_sm_config c = probe_spi_program_get_default_config(off_drv);
    sm_config_set_out_pins(&c, PIN_MOSI, 1);
    sm_config_set_sideset_pins(&c, PIN_CS);
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(pio, SM_DRV, off_drv, &c);
    // pio_sm_init's jump carries a side-set of 0, which leaves CS low.
    pio_sm_exec(pio, SM_DRV, pio_encode_jmp(off_drv + probe_spi_offset_end) | pio_encode_sideset(2, 1));
    phase = h;
}

static void sampler_init(uint sm, uint pin)
{
    pio_sm_config c = probe_sample_program_get_default_config(off_samp);
    sm_config_set_in_pins(&c, pin);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(pio, sm, off_samp, &c);
}

static void sm_reset(uint sm)
{
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    pio_sm_clkdiv_restart(pio, sm);
}

static unsigned timeouts;

// A state machine that never runs would otherwise leave the probe hanging.
static void dma_wait(int ch)
{
    uint64_t end = time_us_64() + 100000;
    while (dma_channel_is_busy((uint)ch)) {
        if (time_us_64() > end) {
            if (timeouts < 4)
                note("timeout on DMA %d with %lu left: pc %u/%u/%u/%u, levels %08lx, fdebug %08lx\n", ch,
                     (unsigned long)dma_hw->ch[ch].transfer_count, pio_sm_get_pc(pio, 0), pio_sm_get_pc(pio, 1),
                     pio_sm_get_pc(pio, 2), pio_sm_get_pc(pio, 3), (unsigned long)pio->flevel,
                     (unsigned long)pio->fdebug);
            dma_channel_abort((uint)ch);
            timeouts++;
            return;
        }
    }
}

static void capture_dma(int ch, uint sm, uint32_t *dst)
{
    dma_channel_config c = dma_channel_get_default_config((uint)ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, false));
    dma_channel_configure((uint)ch, &c, dst, &pio->rxf[sm], CAP_WORDS, true);
}

// One transaction with the probe's SPI master: nw bits from tx, MSB first, then nr clock periods,
// while the other two state machines take MISO and SCK as the PIO sees them on every cycle.
static void probe_xfer(const uint32_t *tx, unsigned nw, unsigned nr)
{
    const uint ss = pio_encode_sideset(2, 1);
    sm_reset(SM_DRV);
    sm_reset(SM_MISO);
    sm_reset(SM_SCK);
    pio_sm_put(pio, SM_DRV, nw - 1);
    pio_sm_exec(pio, SM_DRV, pio_encode_pull(false, true) | ss);
    pio_sm_exec(pio, SM_DRV, pio_encode_out(pio_x, 32) | ss);
    pio_sm_put(pio, SM_DRV, nr);
    pio_sm_exec(pio, SM_DRV, pio_encode_pull(false, true) | ss);
    pio_sm_exec(pio, SM_DRV, pio_encode_out(pio_y, 32) | ss);
    pio_sm_put(pio, SM_DRV, tx[0]);
    pio_sm_exec(pio, SM_DRV, pio_encode_pull(false, true) | ss);
    for (unsigned i = 1; i < (nw + 31) / 32; i++)
        pio_sm_put(pio, SM_DRV, tx[i]);
    pio_sm_exec(pio, SM_DRV, pio_encode_jmp(off_drv + probe_spi_offset_wr) | ss);
    pio_sm_exec(pio, SM_MISO, pio_encode_jmp(off_samp));
    pio_sm_exec(pio, SM_SCK, pio_encode_jmp(off_samp));
    capture_dma(ch_miso, SM_MISO, cap_miso);
    capture_dma(ch_sck, SM_SCK, cap_sck);
    pio_enable_sm_mask_in_sync(pio, (1u << SM_DRV) | (1u << SM_MISO) | (1u << SM_SCK));
    dma_wait(ch_miso);
    dma_wait(ch_sck);
    pio_set_sm_mask_enabled(pio, (1u << SM_DRV) | (1u << SM_MISO) | (1u << SM_SCK), false);
}

static int bit_at(const uint32_t *cap, unsigned c)
{
    return c < CAP_CYCLES ? (int)((cap[c >> 5] >> (31 - (c & 31))) & 1) : 0;
}

// The cycle on which the probe's SPI master drops SCK for read period j.
static unsigned read_fall(unsigned nw, unsigned j)
{
    return 2 * phase * nw + 2 + 2 * phase * j;
}

static void probe_read(uint8_t cmd, uint32_t addr, unsigned dummy, uint8_t *dst, unsigned n)
{
    uint32_t tx[2] = {((uint32_t)cmd << 24) | (addr & 0xffffffu), 0};
    unsigned nw = 32 + dummy;
    probe_xfer(tx, nw, 8 * n);
    memset(dst, 0, n);
    for (unsigned j = 0; j < 8 * n; j++)
        if (bit_at(cap_miso, read_fall(nw, j) + 2 * phase - 1))
            dst[j / 8] |= (uint8_t)(0x80u >> (j % 8));
}

static void probe_write(uint32_t addr, const uint8_t *src, unsigned n)
{
    uint32_t tx[3] = {(0x02u << 24) | (addr & 0xffffffu), 0, 0};
    for (unsigned i = 0; i < n; i++)
        tx[1 + i / 4] |= (uint32_t)src[i] << (24 - 8 * (i % 4));
    probe_xfer(tx, 32 + 8 * n, 0);
}

static void probe_cmd(uint8_t cmd)
{
    uint32_t tx[1] = {(uint32_t)cmd << 24};
    probe_xfer(tx, 8, 0);
}

static void probe_read_block(uint32_t addr, uint8_t *dst, unsigned n)
{
    for (unsigned i = 0; i < n; i += 8)
        probe_read(0x03, addr + i, 0, dst + i, n - i < 8 ? n - i : 8);
}

static void probe_write_block(uint32_t addr, const uint8_t *src, unsigned n)
{
    for (unsigned i = 0; i < n; i += 8)
        probe_write(addr + i, src + i, n - i < 8 ? n - i : 8);
}

static unsigned phase_for(uint32_t khz)
{
    // About 32 ns a phase: long enough to see any delay, short enough to keep CS low under 8 us.
    unsigned h = (khz + 31999) / 32000;
    return h < 3 ? 3 : h > 8 ? 8 : h;
}

static void set_pads(bool fast)
{
    const uint pins[] = {PIN_CS, PIN_SCK, PIN_MOSI};
    for (unsigned i = 0; i < 3; i++) {
        gpio_set_drive_strength(pins[i], fast ? GPIO_DRIVE_STRENGTH_12MA : GPIO_DRIVE_STRENGTH_4MA);
        gpio_set_slew_rate(pins[i], fast ? GPIO_SLEW_RATE_FAST : GPIO_SLEW_RATE_SLOW);
    }
}

#define HOFF 4
#define HBINS 24
typedef struct {
    uint32_t n[HBINS];
} hist_t;

static void hadd(hist_t *h, int l)
{
    int i = l + HOFF;
    h->n[i < 0 || i >= HBINS - 1 ? HBINS - 1 : i]++;
}

static void hstr(char *out, size_t len, const hist_t *h)
{
    size_t k = 0;
    out[0] = 0;
    for (int i = 0; i < HBINS && k < len; i++) {
        if (!h->n[i])
            continue;
        if (i == HBINS - 1)
            k += (size_t)snprintf(out + k, len - k, "%snone:%lu", k ? " " : "", (unsigned long)h->n[i]);
        else
            k += (size_t)snprintf(out + k, len - k, "%s%d:%lu", k ? " " : "", i - HOFF, (unsigned long)h->n[i]);
    }
    if (!k)
        snprintf(out, len, "-");
}

// The most common latency, or 99 when nothing was seen.
static int hmode(const hist_t *h)
{
    int best = -1;
    for (int i = 0; i < HBINS - 1; i++)
        if (h->n[i] && (best < 0 || h->n[i] > h->n[best]))
            best = i;
    return best < 0 ? 99 : best - HOFF;
}

static int first_seen(const uint32_t *cap, unsigned from, int val, unsigned limit)
{
    for (unsigned k = 0; k < limit; k++)
        if (bit_at(cap, from + k) == val)
            return (int)k;
    return 99;
}

static void dump_capture(const char *label)
{
    const uint32_t *caps[2] = {cap_sck, cap_miso};
    const char *names[2] = {"sck", "miso"};
    for (int k = 0; k < 2; k++) {
        note("raw %s %s (bit 31 of word 0 is cycle 0):", label, names[k]);
        for (int i = 0; i < CAP_WORDS; i++)
            note("%s%08lx", i % 8 ? " " : "\n    ", (unsigned long)caps[k][i]);
        note("\n");
    }
}

// Fast-reads 0x55s, whose bits all differ from their neighbours, and records how many cycles each
// SCK edge and each MISO change take to reach the PIO's inputs.
static void measure(uint32_t khz, bool bypass, bool fast)
{
    hist_t sf = {0}, sr = {0}, mi = {0}, rel = {0};
    unsigned bad = 0;
    const unsigned nw = 40, nr = 24;
    set_pads(fast);
    pio->input_sync_bypass = bypass ? (1u << PIN_MISO) | (1u << PIN_SCK) : 0;
    for (int k = 0; k < CAPTURES; k++) {
        uint32_t tx[2] = {(0x0bu << 24) | ADDR_ALT, 0};
        probe_xfer(tx, nw, nr);
        for (unsigned i = 1; i < nw; i++) {
            hadd(&sf, first_seen(cap_sck, 2 * phase * i, 0, 2 * phase));
            hadd(&sr, first_seen(cap_sck, 2 * phase * i + phase, 1, 2 * phase));
        }
        int prev = -1;
        for (unsigned j = 0; j < nr; j++) {
            unsigned f = read_fall(nw, j);
            int ls = first_seen(cap_sck, f, 0, 2 * phase);
            hadd(&sf, ls);
            hadd(&sr, first_seen(cap_sck, f + phase, 1, 2 * phase));
            int b = bit_at(cap_miso, f + 2 * phase - 1);
            if (b != ((0x55 >> (7 - j % 8)) & 1))
                bad++;
            if (prev >= 0 && b != prev) {
                int lm = first_seen(cap_miso, f, b, 2 * phase);
                hadd(&mi, lm);
                if (lm != 99 && ls != 99)
                    hadd(&rel, lm - ls);
            }
            prev = b;
        }
    }
    char a[96], b[96], c[96], d[96];
    hstr(a, sizeof(a), &sf);
    hstr(b, sizeof(b), &sr);
    hstr(c, sizeof(c), &mi);
    hstr(d, sizeof(d), &rel);
    note("%3lu %u %-4s %-4s | %-12s | %-12s | %-16s | %-16s | %u\n", (unsigned long)(khz / 1000), phase,
         bypass ? "byp" : "sync", fast ? "12mA" : "4mA", a, b, c, d, bad);
    if (!fast && (khz == 150000 || (khz == 252000 && bypass))) {
        char label[32];
        snprintf(label, sizeof(label), "%lu %s", (unsigned long)(khz / 1000), bypass ? "byp" : "sync");
        dump_capture(label);
    }
    if (bypass && !fast)
        printf("%3lu MHz sck %d miso %d (+%d)%s\n", (unsigned long)(khz / 1000), hmode(&sf), hmode(&mi), hmode(&rel),
               bad ? " errors" : "");
}

static void lib_start(bool fudge, float div)
{
    uint off = fudge ? off_fudge : off_plain;
    pio_sm_config c = fudge ? spi_psram_fudge_program_get_default_config(off) : spi_psram_program_get_default_config(off);
    sm_config_set_out_pins(&c, PIN_MOSI, 1);
    sm_config_set_in_pins(&c, PIN_MISO);
    sm_config_set_sideset_pins(&c, PIN_CS);
    sm_config_set_out_shift(&c, false, true, 8);
    sm_config_set_in_shift(&c, false, true, 8);
    sm_config_set_clkdiv(&c, div);
    pio_sm_init(pio, SM_LIB, off, &c);
    pio_sm_set_enabled(pio, SM_LIB, true);
}

static void lib_stop(bool fudge)
{
    uint off = fudge ? off_fudge : off_plain;
    for (int i = 0; i < 100000 && !(pio_sm_is_tx_fifo_empty(pio, SM_LIB) && pio_sm_get_pc(pio, SM_LIB) == off); i++)
        tight_loop_contents();
    pio_sm_set_enabled(pio, SM_LIB, false);
}

// As rp2040-psram's pio_spi_write_read_dma_blocking.
static void lib_xfer(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len)
{
    dma_channel_config w = dma_channel_get_default_config((uint)ch_tx);
    channel_config_set_transfer_data_size(&w, DMA_SIZE_8);
    channel_config_set_read_increment(&w, true);
    channel_config_set_write_increment(&w, false);
    channel_config_set_dreq(&w, pio_get_dreq(pio, SM_LIB, true));
    dma_channel_config r = dma_channel_get_default_config((uint)ch_rx);
    channel_config_set_transfer_data_size(&r, DMA_SIZE_8);
    channel_config_set_read_increment(&r, false);
    channel_config_set_write_increment(&r, true);
    channel_config_set_dreq(&r, pio_get_dreq(pio, SM_LIB, false));
    // The command bytes must be in memory before the DMA starts; the compiler may otherwise sink
    // the stores past the register writes.
    __compiler_memory_barrier();
    if (dst_len)
        dma_channel_configure((uint)ch_rx, &r, dst, &pio->rxf[SM_LIB], dst_len, true);
    dma_channel_configure((uint)ch_tx, &w, &pio->txf[SM_LIB], src, src_len, true);
    dma_wait(ch_tx);
    if (dst_len)
        dma_wait(ch_rx);
}

static void lib_read(uint32_t addr, uint8_t *dst, unsigned n)
{
    uint8_t cmd[] = {40, (uint8_t)(8 * n), 0x0b, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, 0};
    lib_xfer(cmd, sizeof(cmd), dst, n);
}

static void lib_write(uint32_t addr, const uint8_t *src, unsigned n)
{
    uint8_t cmd[6 + 32] = {(uint8_t)(32 + 8 * n), 0, 0x02, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};
    memcpy(cmd + 6, src, n);
    lib_xfer(cmd, 6 + n, NULL, 0);
}

static int get_bit(const uint8_t *p, int k)
{
    return (p[k / 8] >> (7 - k % 8)) & 1;
}

// The s for which every bit k read is bit k+s of what was stored, or 99.
static int stream_shift(const uint8_t *got, const uint8_t *want, unsigned n)
{
    const int bits = (int)(8 * n);
    for (int d = 0; d <= 3; d++) {
        for (int sign = 1; sign >= -1; sign -= 2) {
            int s = d * sign;
            bool ok = true;
            for (int k = s < 0 ? -s : 0; k < (s > 0 ? bits - s : bits) && ok; k++)
                ok = get_bit(got, k) == get_bit(want, k + s);
            if (ok)
                return s;
            if (!d)
                break;
        }
    }
    return 99;
}

static const char *shift_name(int s)
{
    switch (s) {
    case 0: return "ok";
    case 1: return "late 1";
    case 2: return "late 2";
    case 3: return "late 3";
    case -1: return "early 1";
    case -2: return "early 2";
    case -3: return "early 3";
    default: return "bad";
    }
}

static void lib_test(uint32_t khz, float div, bool fudge, unsigned index)
{
    uint8_t got[RAND_LEN], data[32], back[32];
    int shift = 0;
    pio->input_sync_bypass = 1u << PIN_MISO;
    lib_start(fudge, div);
    for (unsigned i = 0; i < RAND_LEN; i += 16)
        lib_read(ADDR_RAND + i, got + i, 16);
    for (unsigned i = 0; i < RAND_LEN; i += 16) {
        int s = stream_shift(got + i, rand_data + i, 16);
        if (i == 0)
            shift = s;
        else if (s != shift)
            shift = 99;
    }
    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = rnd8();
    uint32_t waddr = ADDR_WRITE + 64 * index;
    for (unsigned i = 0; i < sizeof(data); i += 16)
        lib_write(waddr + i, data + i, 16);
    lib_stop(fudge);
    probe_read_block(waddr, back, sizeof(back));
    unsigned wrong = 0;
    for (unsigned i = 0; i < sizeof(data); i++)
        wrong += back[i] != data[i];
    char wr[32];
    if (wrong)
        snprintf(wr, sizeof(wr), "%u/32 bad", wrong);
    else
        snprintf(wr, sizeof(wr), "ok");
    say("%3lu %.1f %-5s rd %-7s wr %s\n", (unsigned long)(khz / 1000), (double)div, fudge ? "fudge" : "plain",
        shift_name(shift), wr);
    if (shift != 0) {
        note("    read:");
        for (unsigned i = 0; i < 16; i++)
            note(" %02x", got[i]);
        note("\n    want:");
        for (unsigned i = 0; i < 16; i++)
            note(" %02x", rand_data[i]);
        note("\n");
    }
}

static void pins_init(void)
{
    const uint32_t outs = (1u << PIN_CS) | (1u << PIN_SCK) | (1u << PIN_MOSI);
    pio_gpio_init(pio, PIN_CS);
    pio_gpio_init(pio, PIN_SCK);
    pio_gpio_init(pio, PIN_MOSI);
    pio_gpio_init(pio, PIN_MISO);
    pio_sm_set_pins_with_mask(pio, SM_DRV, 1u << PIN_CS, outs);
    pio_sm_set_pindirs_with_mask(pio, SM_DRV, outs, outs | (1u << PIN_MISO));
}

static const uint32_t sweep_khz[] = {96000, 120000, 132000, 144000, 150000, 168000, 180000,
                                     192000, 200000, 216000, 228000, 240000, 252000};
static const uint32_t lib_khz[] = {120000, 150000, 200000, 252000};
static const float lib_div[] = {1.0f, 1.5f, 2.0f};

int main(void)
{
    base_vreg = vreg_get_voltage();
    // This also moves clk_peri onto the USB PLL, so the screen, SD card and keyboard keep their
    // speeds while the system clock is swept.
    set_clock(150000);
    picocalc_init();

    printf("\033c\033[1mPSRAM probe v" VERSION "\033[0m\n");
    note("PicoCalc mainboard PSRAM probe v" VERSION "\n");
    note("Please send this whole file back.\n\n");
    note("chip: RP2350%s, chip id %08lx, regulator %u mV at start\n", (sysinfo_hw->package_sel & 1) ? "A" : "B",
         (unsigned long)sysinfo_hw->chip_id, vreg_mv(base_vreg));

    off_samp = (uint)pio_add_program(pio, &probe_sample_program);
    off_fudge = (uint)pio_add_program(pio, &spi_psram_fudge_program);
    off_plain = (uint)pio_add_program(pio, &spi_psram_program);
    load_driver(phase_for(150000));
    sampler_init(SM_MISO, PIN_MISO);
    sampler_init(SM_SCK, PIN_SCK);
    ch_miso = dma_claim_unused_channel(true);
    ch_sck = dma_claim_unused_channel(true);
    ch_tx = dma_claim_unused_channel(true);
    ch_rx = dma_claim_unused_channel(true);
    pins_init();
    set_pads(false);
    pio->input_sync_bypass = 1u << PIN_MISO;

    probe_cmd(0x66);
    busy_wait_us(50);
    probe_cmd(0x99);
    busy_wait_us(100);

    uint8_t id[8];
    probe_read(0x9f, 0, 0, id, sizeof(id));
    say("ID:");
    for (unsigned i = 0; i < sizeof(id); i++)
        say(" %02x", id[i]);
    say("\n");

    for (unsigned i = 0; i < RAND_LEN; i++)
        rand_data[i] = rnd8();
    uint8_t alt[8], got[RAND_LEN];
    memset(alt, 0x55, sizeof(alt));
    probe_write_block(ADDR_ALT, alt, sizeof(alt));
    probe_write_block(ADDR_RAND, rand_data, RAND_LEN);
    probe_read_block(ADDR_RAND, got, RAND_LEN);
    unsigned wrong = 0;
    for (unsigned i = 0; i < RAND_LEN; i++)
        wrong += got[i] != rand_data[i];
    say("slow write and read: %s\n", wrong ? "FAILED" : "ok");
    if (wrong) {
        note("    read:");
        for (unsigned i = 0; i < 16; i++)
            note(" %02x", got[i]);
        note("\n");
    }
    // Where the data starts after each read command, in bits relative to the datasheet.
    probe_read(0x03, ADDR_RAND, 0, got, 8);
    int s03 = stream_shift(got, rand_data, 8);
    probe_read(0x0b, ADDR_RAND, 8, got, 8);
    int s0b = stream_shift(got, rand_data, 8);
    say("data start: 03 %s, 0b %s\n", shift_name(s03), shift_name(s0b));
    save_report();

    say("Latency sweep (PIO cycles):\n");
    note("\nLatency in PIO cycles from the side-set that moves SCK to the first `in` that sees the result.\n");
    note("sck_fall/sck_rise: SCK read back on its own pin. miso: the PSRAM's output after a falling edge.\n");
    note("miso-sck: miso less sck_fall for the same edge. Counts after each colon. Last column: wrong bits.\n");
    note("MHz H pads      | sck_fall     | sck_rise     | miso             | miso-sck         | bad\n");
    for (unsigned i = 0; i < sizeof(sweep_khz) / sizeof(sweep_khz[0]); i++) {
        if (!set_clock(sweep_khz[i])) {
            note("%3lu not available\n", (unsigned long)(sweep_khz[i] / 1000));
            continue;
        }
        load_driver(phase_for(sweep_khz[i]));
        for (int byp = 1; byp >= 0; byp--)
            for (int fast = 0; fast <= 1; fast++)
                measure(sweep_khz[i], byp, fast);
        set_pads(false);
        save_report();
    }

    say("rp2040-psram programs:\nMHz div prog\n");
    note("\nrp2040-psram programs, as picocalc_lua runs them (MISO synchroniser bypassed, 4 mA pads):\n");
    note("read: 4 x 16 bytes with 0x0b (late n: each bit read is the one n after it); write: 2 x 16 bytes, read back slowly.\n");
    unsigned index = 0;
    for (unsigned i = 0; i < sizeof(lib_khz) / sizeof(lib_khz[0]); i++) {
        if (!set_clock(lib_khz[i])) {
            note("%3lu not available\n", (unsigned long)(lib_khz[i] / 1000));
            continue;
        }
        load_driver(phase_for(lib_khz[i]));
        for (unsigned d = 0; d < sizeof(lib_div) / sizeof(lib_div[0]); d++)
            for (int fudge = 1; fudge >= 0; fudge--)
                lib_test(lib_khz[i], lib_div[d], fudge, index++);
        save_report();
    }

    set_clock(150000);
    note("\nDMA timeouts: %u\nend of report\n", timeouts);
    if (save_report())
        printf("\n\033[1mSaved " REPORT_FILE "\033[0m\n");
    else
        printf("\n\033[1mCould not save " REPORT_FILE "\033[0m\n");
    while (true)
        tight_loop_contents();
}
