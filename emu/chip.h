#pragma once

#include <pthread.h>
#include <stdatomic.h>

#include "emu.h"

// The RP2350: two Cortex-M33 cores, 520 KB of SRAM, QSPI flash and optional PSRAM behind the QMI,
// and the peripherals PicoCalc firmware uses.

#define ROM_SIZE 0x8000u
#define XIP_BASE 0x10000000u
#define XIP_CS1_BASE 0x11000000u
#define XIP_SRAM_BASE 0x13ffc000u
#define XIP_NOCACHE_BASE 0x14000000u
#define XIP_MAINT_BASE 0x18000000u
#define XIP_UNTRANSLATED_BASE 0x1c000000u
#define SRAM_BASE 0x20000000u
#define SRAM_SIZE 0x82000u
#define USB_DPRAM_BASE 0x50100000u
#define USB_DPRAM_SIZE 0x1000u
#define BOOTRAM_SIZE 0x400u

#define NUM_GPIO_MAX 48
#define NUM_IRQ 52

enum {
    IRQ_TIMER0_0 = 0,
    IRQ_TIMER1_0 = 4,
    IRQ_PWM_WRAP_0 = 8,
    IRQ_DMA_0 = 10,
    IRQ_USBCTRL = 14,
    IRQ_PIO0_0 = 15,
    IRQ_IO_BANK0 = 21,
    IRQ_IO_BANK0_NS = 22,
    IRQ_IO_QSPI = 23,
    IRQ_SIO_FIFO = 25,
    IRQ_SIO_BELL = 26,
    IRQ_SIO_FIFO_NS = 27,
    IRQ_SIO_BELL_NS = 28,
    IRQ_SIO_MTIMECMP = 29,
    IRQ_CLOCKS = 30,
    IRQ_SPI0 = 31,
    IRQ_SPI1 = 32,
    IRQ_UART0 = 33,
    IRQ_UART1 = 34,
    IRQ_ADC_FIFO = 35,
    IRQ_I2C0 = 36,
    IRQ_I2C1 = 37,
    IRQ_OTP = 38,
    IRQ_TRNG = 39,
    IRQ_POWMAN_POW = 44,
    IRQ_POWMAN_TIMER = 45,
    // A line the RP2350 leaves spare, for the RP2040's RTC.
    IRQ_RTC = 51,
};

enum {
    DREQ_PIO0_TX0 = 0,
    DREQ_PIO0_RX0 = 4,
    DREQ_SPI0_TX = 24,
    DREQ_SPI0_RX,
    DREQ_SPI1_TX,
    DREQ_SPI1_RX,
    DREQ_UART0_TX,
    DREQ_UART0_RX,
    DREQ_UART1_TX,
    DREQ_UART1_RX,
    DREQ_PWM_WRAP0 = 32,
    DREQ_I2C0_TX = 44,
    DREQ_I2C0_RX,
    DREQ_I2C1_TX,
    DREQ_I2C1_RX,
    DREQ_ADC,
    DREQ_XIP_STREAM,
    DREQ_XIP_QMITX,
    DREQ_XIP_QMIRX,
    DREQ_SHA256 = 54,
    DREQ_COUNT = 64,
    DREQ_DMA_TIMER0 = 59,
    DREQ_FORCE = 63,
};

enum {
    FUNC_HSTX = 0,
    FUNC_SPI = 1,
    FUNC_UART = 2,
    FUNC_I2C = 3,
    FUNC_PWM = 4,
    FUNC_SIO = 5,
    FUNC_PIO0 = 6,
    FUNC_PIO1 = 7,
    FUNC_PIO2 = 8,
    FUNC_XIP_CS1 = 9,
    FUNC_USB = 10,
    FUNC_UART_AUX = 11,
    FUNC_NULL = 0x1f,
};

#define PIN_HIST 8

typedef struct {
    uint32_t ctrl;
    bool ext_driven;
    bool ext_level;
    bool level;
    // `level` reaches the PIO's inputs a few nanoseconds after it changes at the pad, so its recent
    // changes are kept with the time each arrives: a ring, newest at hist_new, and the level
    // before the oldest one kept.
    uint8_t hist_new, hist_n, hist_lvl;
    bool hist_base;
    uint64_t hist_t[PIN_HIST];
} gpio_pin_t;

enum { WAVE_LEVEL, WAVE_PWM };

// A pin's output over time: a steady level, or a running PWM channel that is high while its
// counter is below `high` (on both slopes when phase correct), inverted when `level` is set.
typedef struct {
    uint8_t kind;
    bool level;
    bool phase_correct;
    uint32_t ctr, period, high;
    uint64_t t0, tick;
} pin_wave_t;

typedef void (*gpio_watch_fn)(void *ctx, unsigned pin, bool level);

typedef struct gpio_watch {
    gpio_watch_fn fn;
    void *ctx;
    struct gpio_watch *next;
} gpio_watch_t;

typedef struct {
    uint32_t cr0, cr1, cpsr, imsc, ris, dmacr;
    uint16_t txf[8], rxf[8];
    int txn, rxn, txh, rxh;
    uint64_t busy_until;
    emu_event_t ev;
    int idx;
} pl022_t;

typedef struct {
    uint32_t con, tar, sar, ss_hcnt, ss_lcnt, fs_hcnt, fs_lcnt, intr_mask, rx_tl, tx_tl, enable, sda_hold,
        slv_data_nack, dma_cr, dma_tdlr, dma_rdlr, sda_setup, ack_general_call, fs_spklen;
    uint8_t rxf[16];
    int rxn, rxh;
    int txn;
    uint32_t raw_intr, abrt_source;
    bool active, in_read;
    uint8_t wbuf[256];
    int wlen;
    uint8_t rbuf[64];
    int rpos;
    void *dev;
    uint64_t busy_until;
    bool stop_pending;
    emu_event_t ev;
    int idx;
} dw_i2c_t;

typedef struct {
    uint32_t ibrd, fbrd, lcr_h, cr, ifls, imsc, ris, dmacr;
    uint16_t rxf[32];
    int rxn, rxh;
    int txn;
    uint64_t tx_done;
    emu_event_t ev;
    int idx;
} pl011_t;

typedef struct {
    uint32_t clkdiv, execctrl, shiftctrl, pinctrl;
    uint32_t pc, x, y, isr, osr;
    int isr_count, osr_count;
    uint32_t txf[8], rxf[8];
    int txn, rxn, txh, rxh;
    int delay;
    bool stalled;
    // An IN filled the ISR while the RX FIFO was full; it stays stalled until the push goes through.
    bool push_stall;
    uint32_t div_acc;
    uint64_t next_t;
} pio_sm_t;

typedef struct {
    uint32_t ctrl, fdebug, irq, input_sync_bypass, gpiobase;
    uint16_t instr[32];
    pio_sm_t sm[4];
    uint32_t inte[2], intf[2];
    uint64_t out_val, out_en, pin_owned;
    uint64_t t;
    int idx;
    // Set while the state machines catch up, during which DMA they wake may read or write the FIFOs.
    bool syncing;
    emu_event_t ev;
} pio_t;

typedef struct {
    uint32_t read_addr, write_addr, trans_count, ctrl;
    uint32_t count_reload;
} dma_ch_t;

typedef struct {
    dma_ch_t ch[16];
    uint32_t intr, inte[4], intf[4];
    uint32_t timer[4];
    uint32_t sniff_ctrl, sniff_data;
    uint32_t seccfg[16], seccfg_irq[4], seccfg_misc, misc[8];
    emu_event_t ev;
    bool running;
} dma_t;

typedef struct {
    uint32_t csr, div, ctr, cc, top;
    uint64_t t0;
} pwm_slice_t;

typedef struct {
    pwm_slice_t s[12];
    uint32_t en, intr, inte[2], intf[2];
} pwm_t;

typedef struct {
    uint32_t accum[2], base[3], ctrl[2];
} interp_t;

typedef struct {
    uint32_t data[8];
    int head, count;
    bool roe, wof;
} sio_fifo_t;

// What the SIO duplicates for Non-secure use: one core's FIFO to the other, by sending core.
typedef struct {
    sio_fifo_t fifo[2];
    uint32_t spinlocks;
    uint32_t doorbell[2];
} sio_bank_t;

typedef struct {
    uint32_t ctrl, div;
} clock_reg_t;

typedef struct {
    uint32_t cs, pwr, fbdiv, prim, intr, inte, intf;
} pll_t;

typedef struct {
    uint64_t base_us, base_t;
    uint32_t latch_hi;
    uint32_t alarm[4], armed, intr, inte, intf, pause, dbgpause, locked, source;
    emu_event_t ev[4];
    int idx;
    // The tick timer_us last found (q ticks of q_period after q_base, starting q_dt in), which
    // most reads land in again.
    uint64_t q_base, q_period, q_dt, q;
} timer_t_;

typedef struct {
    uint32_t direct_csr, direct_rx[4], direct_tx[4];
    int rxn, txn;
    // The flash reports itself busy until then, after an erase or program.
    uint64_t flash_busy_until;
    uint32_t m[2][5];
    uint32_t atrans[8];
    bool cs[2];
    uint8_t cmd[16];
    int cmd_len;
    int phase;
    uint32_t addr;
    uint8_t sr1, sr2, sr3;
    bool wel, psram_qpi;
    uint64_t busy_until;
} qmi_t;

typedef enum {
    BOARD_PICO2,
    BOARD_PICO2_W,
    BOARD_PIMORONI_PICO_PLUS2_W,
    BOARD_PIMORONI_PICO_PLUS2,
    BOARD_PICO,
    BOARD_PICO_W,
} board_t;

typedef struct {
    board_t id;
    const char *name;
    const char *sdk_name;
    uint32_t flash_size;
    uint32_t psram_size;
    unsigned num_gpio;
    int psram_cs_pin;
    bool wifi;
    bool rp2040;
} board_info_t;

// The DW_apb_ssi the RP2040 reaches its flash through, for commands and for XIP.
typedef struct {
    uint32_t ctrlr0, ctrlr1, ssienr, ser, baudr, txftlr, rxftlr, imr, dmacr, dmatdlr, dmardlr;
    uint32_t rx_sample_dly, spi_ctrlr0, txd_drive_edge;
    uint32_t rxf[16];
    int rxn, rxh;
    bool rx_over;
    // Frames written for an EEPROM-mode read that has not started yet.
    uint32_t eeprom_tx[2];
    int eeprom_n;
    // The flash is selected by the SSI itself until this time, when the frames it has been sent
    // have finished shifting out.
    bool auto_cs;
    uint64_t busy_until;
    // Whether XIP reads get flash data with the present settings, and what a cache miss costs.
    bool xip_ok;
    uint32_t miss_cyc;
} ssi_t;

// State only the RP2040 has, or keeps in its own layout.
typedef struct {
    uint32_t resets;
    uint32_t gpio_ctrl[30];
    uint32_t qspi_ctrl[6];
    uint32_t pads_qspi[7];
    uint32_t clk_div[10];
    uint32_t wd_tick;
    uint32_t vreg, bod, chip_reset;
    uint32_t syscfg[8];
    uint32_t xip_ctrl;
    struct {
        uint32_t dividend, divisor, quotient, remainder;
        bool dirty;
    } div[2];
    uint32_t sio_hi_out, sio_hi_oe;
    uint32_t busctrl[10];
    uint32_t dma_ctrl[12];
    uint32_t psm_frce_on, psm_frce_off, psm_wdsel;
    uint32_t xip_stream_addr, xip_stream_ctr;
    uint32_t rtc[12];
    uint64_t rtc_base_t;
    int64_t rtc_base_s;
    uint64_t rtc_hz;
    ssi_t ssi;
    // The flash is in continuous read mode, taking each selection as the address of a quad read.
    bool flash_crm;
} rp2040_t;

// The USB controller's registers and the state behind them, for device mode; host mode keeps its
// registers but moves nothing on the bus.
typedef struct {
    uint32_t addr_endp[16];
    uint32_t main_ctrl, sof_wr, sof_rd, sie_ctrl, sie_status, int_ep_ctrl;
    uint32_t buff_status, buff_which, ep_abort, ep_abort_done, ep_stall_arm, nak_poll, stall_nak;
    uint32_t muxing, pwr, phy_direct, phy_override, phy_trim, linestate_tuning, inte, intf;
    uint32_t sof_ts_last, ep_tx_error, ep_rx_error, sm_watchdog;
    // INTR sources that have no SIE_STATUS bit of their own to clear.
    bool sof_irq, conn_irq, susp_irq;
    bool connected, suspended, listening, pullup;
    unsigned line;
    // Each double-buffered endpoint's buffer select; a second finished buffer waiting for its
    // BUFF_STATUS bit to be cleared, and which buffer it was; and the odd buffers of endpoints that
    // interrupt every two.
    uint32_t buf_sel, queued, queued_which, odd_done;
    // When the bus last carried anything, which suspends the device 3 ms later.
    uint64_t activity_t;
    emu_event_t idle_ev;
    // The far end of the cable: whether a host is there and what it drives.
    bool host_present, host_reset;
    struct usb_host *host;
} usb_t;

// The RP2350 clock generators, in register order.
enum { CLK_GPOUT0, CLK_GPOUT1, CLK_GPOUT2, CLK_GPOUT3, CLK_REF, CLK_SYS, CLK_PERI, CLK_HSTX, CLK_USB, CLK_ADC };

struct chip {
    cpu_t core[2];
    const board_info_t *board;

    uint8_t rom[0x10000];
    uint8_t sram[0x90000];
    uint8_t xip_sram[0x4000];
    uint8_t usb_dpram[USB_DPRAM_SIZE];
    uint8_t bootram[BOOTRAM_SIZE];
    uint8_t *flash;
    uint32_t flash_size;
    uint8_t *psram;
    uint32_t psram_size;
    uint8_t *rpage[0x10000];
    uint8_t *wpage[0x10000];
    // rpage without the XIP windows, whose reads also cost a cache lookup.
    uint8_t *rpage_plain[0x10000];
    bool flash_dirty;
    uint32_t flash_dirty_lo, flash_dirty_hi;

    uint64_t now;
    // Time of the pin change in progress when it is not `now` (a core's own time, or a PIO state
    // machine's as it catches up), or 0.
    uint64_t edge_t;
    uint32_t sys_ps;
    uint64_t sys_hz, ref_hz, peri_hz, usb_hz, adc_hz, hstx_hz, pll_sys_hz, pll_usb_hz, xosc_hz, rosc_hz;
    emu_event_t *events;
    uint64_t irq_lines;
    uint64_t irq_core[2];
    // The same lines numbered as the cores' NVICs see them, which differs on the RP2040.
    uint64_t irq_ext;
    uint64_t irq_core_ext[2];
    bool dreq[DREQ_COUNT];

    uint32_t resets;
    uint32_t psm_frce_on, psm_frce_off, psm_wdsel;
    clock_reg_t clk[10];
    uint32_t clk_resus, fc0[8], clk_wake[2], clk_sleep[2], clk_inte, clk_intf;
    uint32_t xosc_ctrl, xosc_startup, xosc_count;
    uint64_t xosc_count_t;
    uint32_t rosc_regs[10];
    pll_t pll_sys, pll_usb;
    uint32_t syscfg[8];
    uint32_t busctrl[12];
    uint32_t accessctrl[64];
    // Both timers take any Secure access from either core, so bus_read's quick path to them need
    // not filter; and every endpoint takes Secure privileged accesses from either core, also when
    // the SAU makes memory Non-secure, so those need no filtering at all.
    bool ac_timer_open, ac_sp_open;
    // Who made the bus access a peripheral is handling, for the ones that filter by its security and
    // privilege themselves: a core, or else the DMA or the simulator at the given SECCFG level.
    cpu_t *access_core;
    uint32_t access_seccfg;
    uint32_t ticks[18];
    // tick_ps's last answer for each tick generator, keyed on what it was computed from.
    struct {
        uint32_t cycles, ref_hz;
        uint64_t ps;
    } tick_memo[6];
    uint32_t powman[64];
    uint64_t aon_base_ms, aon_base_t;
    uint32_t otp_regs[0x60];
    uint32_t bootlock_stat;
    uint32_t bootram_write_once[2];
    uint32_t trng_regs[8];
    // What ROSC's random bit and the TRNG read: the same sequence from every power-up.
    uint64_t rng;
    uint32_t glitch[16];
    struct {
        uint32_t csr, sum[8], block[16], partial;
        unsigned words, partial_bytes;
    } sha256;
    // The redundancy coprocessor of each core.
    struct {
        uint64_t salt[2];
        bool salt_valid[2], fault[2];
        uint8_t count[2];
    } rcp;
    uint32_t hstx_regs[16];

    uint32_t wd_ctrl, wd_reason, wd_scratch[8];
    uint32_t wd_load;
    uint64_t wd_t0;
    emu_event_t wd_ev;

    timer_t_ timer[2];

    gpio_pin_t gpio[NUM_GPIO_MAX];
    gpio_pin_t qspi[6];
    uint32_t pads[NUM_GPIO_MAX + 3];
    uint32_t pads_qspi[7];
    uint32_t gpio_inte[2][6], gpio_intf[2][6];
    uint32_t gpio_edge[6];
    uint64_t gpio_irq_pins;
    // gpio_in_all's answer, recomputed after anything that changes a pin's level, input
    // override or pad.
    uint64_t gpio_in_cache;
    bool gpio_in_valid;
    // Pins with changes in their history, and the latest time any of them reaches the inputs.
    uint64_t in_late, in_settle_t;
    // While a pin change is handled: when it reached the pad, as watching devices see it. A device
    // driving a pin from its watch callback sets ext_at to when its own output changes, or leaves
    // it 0 for at once.
    uint64_t pin_t, ext_at;
    // USB DP and DM, then the QSPI pins: SCLK, SS, SD0 to SD3.
    uint32_t qspi_ctrl[8];
    gpio_watch_t *watch[NUM_GPIO_MAX];

    uint32_t sio_out[2], sio_oe[2];
    // The Secure bank, then the Non-secure one.
    sio_bank_t sio[2];
    interp_t interp[2][2];
    uint32_t peri_nonsec;

    pl022_t spi[2];
    dw_i2c_t i2c[2];
    pl011_t uart[2];
    pio_t pio[3];
    dma_t dma;
    pwm_t pwm;
    qmi_t qmi;

    uint32_t xip_ctrl, xip_ctr_hit, xip_ctr_acc;
    // Changes whenever ROM or flash contents, or where they are mapped, may have changed, so
    // predecoded blocks from them need no byte-by-byte check.
    uint32_t code_gen;
    // Set when every translated block must go, at the next point where none is running.
    bool code_flush;
    uint32_t xip_stream_addr, xip_stream_ctr;
    uint32_t xip_tag[2048][2];
    uint8_t xip_lru[2048];
    // The tag in each set's most recently used way, kept by xip_access so a repeat hit costs one
    // load.
    uint32_t xip_mru[1024];

    uint32_t adc_cs, adc_result, adc_fcs, adc_div, adc_inte, adc_intf;
    usb_t usb;

    struct {
        uint64_t x_bits, y_bits;
        int op;
        bool engaged;
        double result;
        uint64_t raw_result;
    } dcp[2];

    uint64_t boot_count;
    uint32_t boot_type, boot_param[2];
    int rom_partition;

    pthread_mutex_t power_lock;
    bool req_reset, req_bootsel, req_poweroff;
    // Whether any of those is set, for the run loop to check without taking the lock.
    atomic_bool req_any;
    char req_reason[96];
    bool in_bootsel;

    struct cyw43 *wifi;

    bool rp2040;
    // Raspberry Pi's own RP2350 ROM is in rom[] instead of the stand-in rom.c builds.
    bool real_rom;
    // The A3 stepping, whose hardware the A3 and A4 boot ROMs run on; otherwise the chip is an A2.
    bool a3;
    // The BOOTSEL button is held until then, pulling the QSPI chip select low.
    uint64_t bootsel_until;
    // Size of the bus write being passed to a peripheral, which sees only the replicated data.
    int bus_write_size;
    uint32_t sram_size;
    // XIP aliases below this one look in the cache.
    uint8_t xip_cached_top;
    rp2040_t rp;

    bool trace;
};

extern chip_t *g_chip;
extern const board_info_t board_table[];
const board_info_t *board_find(const char *name);

void chip_init(chip_t *m, const board_info_t *b);
// Undoes chip_init, leaving nothing behind that a later chip_init would see.
void chip_free(chip_t *m);
void chip_power_on(chip_t *m);
void chip_reset(chip_t *m, bool watchdog);
void chip_update_clocks(chip_t *m);
void chip_rebuild_pages(chip_t *m);
void chip_irq(chip_t *m, unsigned irq, bool level);
// Interrupts such as the SIO FIFO and GPIO bank that each core's NVIC sees separately.
void chip_irq_core(chip_t *m, int core, unsigned irq, bool level);
void chip_dreq(chip_t *m, unsigned dreq, bool level);
void chip_sev(chip_t *m, int from_core);
void chip_request(chip_t *m, int what, const char *reason);
enum { REQ_RESET, REQ_BOOTSEL, REQ_POWEROFF };

uint32_t bus_read(cpu_t *c, uint32_t addr, int size);
void bus_write(cpu_t *c, uint32_t addr, uint32_t v, int size);
uint16_t bus_fetch16_slow(cpu_t *c, uint32_t addr);
uint32_t sys_read(chip_t *m, uint32_t addr, int size, bool *ok);
void sys_write(chip_t *m, uint32_t addr, uint32_t v, int size, bool *ok);
uint32_t xip_access(chip_t *m, uint32_t addr);

// xip_access for the commonest case inline: a hit on the line used last in its set leaves the
// cache state as it was apart from the counters.
static inline uint32_t xip_cost(chip_t *m, uint32_t addr)
{
    if (likely((addr >> 24) < m->xip_cached_top && (m->xip_ctrl & 1))) {
        uint32_t line = (addr & 0x01ffffffu) >> 3, set = line & 1023;
        if (m->xip_mru[set] == ((line >> 10) | 0x80000000u)) {
            m->xip_ctr_acc++;
            m->xip_ctr_hit++;
            return 0;
        }
    }
    return xip_access(m, addr);
}
uint8_t *xip_translate(chip_t *m, uint32_t addr);

bool coproc_mcr(cpu_t *c, unsigned cp, unsigned opc1, unsigned crn, unsigned crm, unsigned opc2, uint32_t v);
bool coproc_mrc(cpu_t *c, unsigned cp, unsigned opc1, unsigned crn, unsigned crm, unsigned opc2, uint32_t *v);
bool coproc_mcrr(cpu_t *c, unsigned cp, unsigned opc1, unsigned crm, uint32_t lo, uint32_t hi);
bool coproc_mrrc(cpu_t *c, unsigned cp, unsigned opc1, unsigned crm, uint32_t *lo, uint32_t *hi);
bool coproc_cdp(cpu_t *c, unsigned cp, unsigned opc1, unsigned crd, unsigned crn, unsigned crm, unsigned opc2);
void dcp_reset(void);
void rcp_reset(chip_t *m);
void rcp_set_salts(chip_t *m, uint64_t salt0, uint64_t salt1);
void rcp_exec(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2);
void sha256_reset(chip_t *m);
uint32_t sha256_read(chip_t *m, uint32_t off);
void sha256_write(chip_t *m, uint32_t off, uint32_t v);
bool dcp_mcrr(cpu_t *c, unsigned cp, unsigned opc1, unsigned crm, uint32_t lo, uint32_t hi);
bool dcp_mrrc(cpu_t *c, unsigned cp, unsigned opc1, unsigned crm, uint32_t *lo, uint32_t *hi, bool pop);
bool dcp_mrc(cpu_t *c, unsigned cp, unsigned crm, unsigned opc2, uint32_t *v, bool pop);
bool dcp_cdp(cpu_t *c, unsigned cp, unsigned opc1, unsigned crd, unsigned crn, unsigned crm, unsigned opc2);

uint32_t sysctl_read(chip_t *m, uint32_t base, uint32_t off);
void sysctl_write(chip_t *m, uint32_t base, uint32_t off, uint32_t v);
uint32_t timer_read(chip_t *m, timer_t_ *t, uint32_t off, bool peek);
void timer_write(chip_t *m, timer_t_ *t, uint32_t off, uint32_t v);
void timer_reset(chip_t *m, timer_t_ *t, int idx);
void timer_retime(chip_t *m);
uint64_t timer_us(chip_t *m, timer_t_ *t);
uint32_t watchdog_read(chip_t *m, uint32_t off);
void watchdog_write(chip_t *m, uint32_t off, uint32_t v);
void watchdog_retime(chip_t *m);
uint32_t powman_read(chip_t *m, uint32_t off);
void powman_write(chip_t *m, uint32_t off, uint32_t v);
// off is the address less SIO_BASE; an access the SIO refuses clears *ok.
uint32_t sio_read(cpu_t *c, uint32_t off, bool *ok);
void sio_write(cpu_t *c, uint32_t off, uint32_t v, bool *ok);
void sio_reset(chip_t *m);
void sio_fifo_irq(chip_t *m);
void pl022_sync_all(chip_t *m);
extern bool g_fast_bus;
bool gpioc_mcrr(cpu_t *c, unsigned opc1, unsigned crm, uint32_t lo, uint32_t hi);
bool gpioc_mcr(cpu_t *c, unsigned opc1, unsigned crm, unsigned opc2, uint32_t v);
bool gpioc_mrc(cpu_t *c, unsigned opc1, unsigned crm, unsigned opc2, uint32_t *v);
uint32_t iobank_read(chip_t *m, uint32_t off);
void iobank_write(chip_t *m, uint32_t off, uint32_t v);
uint32_t ioqspi_read(chip_t *m, uint32_t off);
void ioqspi_write(chip_t *m, uint32_t off, uint32_t v);
uint32_t pads_read(chip_t *m, bool qspi, uint32_t off);
void pads_write(chip_t *m, bool qspi, uint32_t off, uint32_t v);
void gpio_reset(chip_t *m);
void gpio_refresh(chip_t *m);
void gpio_refresh_mask(chip_t *m, uint64_t mask);
bool gpio_in(chip_t *m, unsigned pin);
int gpio_func(chip_t *m, unsigned pin);
uint64_t gpio_in_all(chip_t *m);
uint64_t gpio_in_at(chip_t *m, uint64_t pins, uint64_t bypass, uint64_t t);
int gpio_pad_speed(chip_t *m, unsigned pin);
bool qspi_ss_override(chip_t *m, bool *level);
void gpio_wave(chip_t *m, unsigned pin, pin_wave_t *w);

// Pins whose waveform a device is recording; see pinwave.c.
extern uint64_t g_wave_pins;
void pinwave_update(chip_t *m, uint64_t pins);
void pinwave_advance(chip_t *m);
void pinwave_stop(void);

void pl022_reset(pl022_t *s, int idx);
uint32_t pl022_read(chip_t *m, pl022_t *s, uint32_t off, bool peek);
void pl022_write(chip_t *m, pl022_t *s, uint32_t off, uint32_t v);
bool pl022_cs_low(chip_t *m, int idx);

void dw_i2c_reset(dw_i2c_t *s, int idx);
uint32_t dw_i2c_read(chip_t *m, dw_i2c_t *s, uint32_t off, bool peek);
void dw_i2c_write(chip_t *m, dw_i2c_t *s, uint32_t off, uint32_t v);

void pl011_reset(pl011_t *s, int idx);
uint32_t pl011_read(chip_t *m, pl011_t *s, uint32_t off, bool peek);
void pl011_write(chip_t *m, pl011_t *s, uint32_t off, uint32_t v);
void pl011_rx(chip_t *m, int idx, uint8_t byte);

void pio_reset(pio_t *p, int idx);
uint32_t pio_read(chip_t *m, pio_t *p, uint32_t off, bool peek);
void pio_write(chip_t *m, pio_t *p, uint32_t off, uint32_t v);
void pio_sync(chip_t *m, pio_t *p);
bool pio_drives(chip_t *m, int idx, unsigned pin, bool *level);

void dma_reset(dma_t *d);
uint32_t dma_read(chip_t *m, uint32_t off, bool peek);
// Returns false for a write the DMA refuses with a bus error.
bool dma_write(chip_t *m, uint32_t off, uint32_t v);
// A DMA channel's transfer, filtered at the channel's security level by ACCESSCTRL.
uint32_t dma_bus_read(chip_t *m, uint32_t addr, int size, uint32_t seccfg, bool *ok);
void dma_bus_write(chip_t *m, uint32_t addr, uint32_t v, int size, uint32_t seccfg, bool *ok);

void accessctrl_reset(chip_t *m, bool full);
uint32_t accessctrl_read(chip_t *m, uint32_t off);
// Returns false for the bus error ACCESSCTRL gives a write it refuses; c is NULL for the DMA.
bool accessctrl_write(chip_t *m, cpu_t *c, uint32_t addr, uint32_t data);
// Whether the bus filters let a core's load or store, or a DMA channel's transfer, through.
bool accessctrl_core_allows(cpu_t *c, uint32_t a);
bool accessctrl_dma_allows(chip_t *m, uint32_t a, uint32_t seccfg);
// The RESETS bits of the peripherals Non-secure code of that privilege can reach.
uint32_t accessctrl_resets_ns_mask(chip_t *m, bool priv);
bool accessctrl_ns_reaches(chip_t *m, unsigned ep, bool priv);
// Whether function f may be selected on a pin, which it may not if the pin is Non-secure and
// the peripheral is not.
bool accessctrl_func_allowed(chip_t *m, unsigned pin, unsigned f);
bool gpio_ns(const chip_t *m, unsigned pin);
void gpio_security_changed(chip_t *m);

static inline bool access_ns(const chip_t *m)
{
    return m->access_core ? m->access_core->bus_ns : !(m->access_seccfg & 2);
}

static inline bool access_priv(const chip_t *m)
{
    return m->access_core ? cpu_privileged(m->access_core) : (m->access_seccfg & 1);
}
void dma_kick(chip_t *m);

void pwm_reset(pwm_t *p);
uint32_t pwm_read(chip_t *m, uint32_t off);
void pwm_write(chip_t *m, uint32_t off, uint32_t v);
void pwm_wave(chip_t *m, unsigned pin, pin_wave_t *w);

void qmi_reset(chip_t *m);
uint32_t qmi_read(chip_t *m, uint32_t off);
void qmi_write(chip_t *m, uint32_t off, uint32_t v);
uint32_t xipctrl_read(chip_t *m, uint32_t off);
void xipctrl_write(chip_t *m, uint32_t off, uint32_t v);
void xip_flush(chip_t *m);
void flash_erase(chip_t *m, uint32_t off, uint32_t len);
void flash_program(chip_t *m, uint32_t off, const uint8_t *src, uint32_t len);

// Answers to a host's transaction: a handshake, or USB_NONE for no answer at all.
enum { USB_ACK, USB_NAK, USB_STALL, USB_NONE };
void usb_reset(chip_t *m);
uint32_t usb_read(chip_t *m, uint32_t off, bool peek);
void usb_write(chip_t *m, uint32_t off, uint32_t v, unsigned alias);
void usb_clocks_changed(chip_t *m);
// The bus as the host drives it. usb_bus_in's buf holds up to 1023 bytes, the largest packet.
void usb_bus_attach(chip_t *m, bool present);
void usb_bus_reset(chip_t *m, bool active);
void usb_bus_sof(chip_t *m, unsigned frame);
int usb_bus_setup(chip_t *m, unsigned addr, const uint8_t pkt[8]);
int usb_bus_in(chip_t *m, unsigned addr, unsigned ep, uint8_t *buf, unsigned *len, unsigned *pid);
int usb_bus_out(chip_t *m, unsigned addr, unsigned ep, unsigned pid, const uint8_t *data, unsigned len);
void usbhost_init(chip_t *m);
void usbhost_free(chip_t *m);
// The device's pull-up has appeared on D+ or gone.
void usbhost_pullup(chip_t *m, bool on);
void usbhost_chip_reset(chip_t *m);
// Queues bytes for the device's serial port while the host has it open, giving how many fit.
size_t usbhost_serial_send(chip_t *m, const uint8_t *data, size_t len);
bool usbhost_serial_is_open(chip_t *m);
// A boot ROM's drive the host has mounted, and copying a UF2's bytes onto it, which it takes and
// frees; one given before the drive mounts waits for it.
bool usbhost_drive_mounted(chip_t *m);
bool usbhost_drive_copy(chip_t *m, uint8_t *data, size_t len, const char *name);
// A terminal came or went: see picocalc_sim_usb_serial_attach.
void usbhost_serial_wanted_changed(chip_t *m);
int usbhost_serial_state(chip_t *m, char *product, size_t cap);

void cyw43_init(chip_t *m);
void cyw43_free(chip_t *m);
void cyw43_reset(chip_t *m);
bool cyw43_bus_pins(chip_t *m, unsigned clk, unsigned data);
void cyw43_bus_out(chip_t *m, int bit);
int cyw43_bus_in(chip_t *m);

void rom_build(chip_t *m);
// Puts Raspberry Pi's RP2350 boot ROM, a 32 KB image, in place of the stand-in.
bool rp2350_load_rom(chip_t *m, const char *path, char *err, size_t errlen);
void rom_hle(cpu_t *c, unsigned n);
void rom_core_start(cpu_t *c);

// The flash behind QMI chip select 0 or the RP2040's SSI, a byte at a time.
void flash_dev_select(chip_t *m, bool selected);
uint8_t flash_dev_xfer(chip_t *m, uint8_t mosi);
bool flash_dev_quad_enabled(chip_t *m);

bool rp2040_load_rom(chip_t *m, const char *path, char *err, size_t errlen);
void rp2040_reset(chip_t *m, bool power_on);
void rp2040_update_clocks(chip_t *m);
uint32_t rp2040_periph_read(chip_t *m, uint32_t addr, bool peek, bool *ok);
void rp2040_periph_write(chip_t *m, uint32_t addr, uint32_t v, bool *ok);
uint32_t rp2040_xip_read(chip_t *m, cpu_t *c, uint32_t addr, int size, bool *ok);
void rp2040_xip_write(chip_t *m, uint32_t addr, uint32_t v, int size, bool *ok);
uint8_t *rp2040_xip_translate(chip_t *m, uint32_t addr);
uint32_t rp2040_dreq_in(uint32_t treq);
int rp2040_irq_line(unsigned irq, int core);
uint32_t rp2040_qspi_in(chip_t *m);
void rp2040_sio_hi_changed(chip_t *m);
uint32_t rp2040_dreq_out(uint32_t treq);
void chip_core1_power(chip_t *m, bool off);

void devices_spi_frame(chip_t *m, int bus, uint16_t mosi, int bits, uint16_t *miso);
void devices_gpio_changed(chip_t *m, unsigned pin, bool level);
// Clocks one bit into the SPI device wired to these pins, for PIO programs run in bulk.
void devices_spi_bit(chip_t *m, unsigned sck_pin, unsigned mosi_pin, int bit);
int devices_i2c_start(chip_t *m, int bus, uint8_t addr, void **dev);
void devices_i2c_write(void *dev, const uint8_t *buf, int len, bool nostop);
int devices_i2c_read(void *dev, uint8_t *buf, int len, bool nostop);
void devices_uart_tx(chip_t *m, int idx, uint8_t byte);
// Drops every device, watch and waveform listener the device models registered.
void devices_detach_all(void);

static inline uint32_t rd32le(const uint8_t *p)
{
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    return v;
}

static inline uint16_t rd16le(const uint8_t *p)
{
    uint16_t v;
    __builtin_memcpy(&v, p, 2);
    return v;
}

static inline void wr32le(uint8_t *p, uint32_t v)
{
    __builtin_memcpy(p, &v, 4);
}

static inline void wr16le(uint8_t *p, uint16_t v)
{
    __builtin_memcpy(p, &v, 2);
}
