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
    IRQ_IO_QSPI = 23,
    IRQ_SIO_FIFO = 25,
    IRQ_SIO_BELL = 26,
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

typedef struct {
    uint32_t ctrl;
    bool ext_driven;
    bool ext_level;
    bool level;
} gpio_pin_t;

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
    uint32_t seccfg[16], misc[8];
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
} timer_t_;

typedef struct {
    uint32_t direct_csr, direct_rx[4];
    int rxn;
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
} board_info_t;

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
    bool flash_dirty;
    uint32_t flash_dirty_lo, flash_dirty_hi;

    uint64_t now;
    uint32_t sys_ps;
    uint64_t sys_hz, ref_hz, peri_hz, usb_hz, adc_hz, hstx_hz, pll_sys_hz, pll_usb_hz, xosc_hz, rosc_hz;
    emu_event_t *events;
    uint64_t irq_lines;
    uint64_t irq_core[2];
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
    uint32_t ticks[18];
    uint32_t powman[64];
    uint64_t aon_base_ms, aon_base_t;
    uint32_t otp_regs[0x60];
    uint32_t bootlock_stat;
    uint32_t bootram_write_once[2];
    uint32_t trng_regs[8];
    uint32_t glitch[16];
    uint32_t sha256_regs[16];
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
    uint32_t qspi_ctrl[6];
    gpio_watch_t *watch[NUM_GPIO_MAX];

    uint32_t sio_out[2], sio_oe[2];
    sio_fifo_t fifo[2];
    uint32_t spinlocks;
    uint32_t doorbell[2];
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
    uint32_t xip_stream_addr, xip_stream_ctr;
    uint32_t xip_tag[2048][2];
    uint8_t xip_lru[2048];

    uint32_t adc_cs, adc_result, adc_fcs, adc_div, adc_inte, adc_intf;
    uint32_t usb_regs[64];

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
    char req_reason[96];
    bool in_bootsel;

    bool trace;
};

extern chip_t *g_chip;
extern const board_info_t board_table[];
const board_info_t *board_find(const char *name);

void chip_init(chip_t *m, const board_info_t *b);
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
uint8_t *xip_translate(chip_t *m, uint32_t addr);

bool coproc_mcr(cpu_t *c, unsigned cp, unsigned opc1, unsigned crn, unsigned crm, unsigned opc2, uint32_t v);
bool coproc_mrc(cpu_t *c, unsigned cp, unsigned opc1, unsigned crn, unsigned crm, unsigned opc2, uint32_t *v);
bool coproc_mcrr(cpu_t *c, unsigned cp, unsigned opc1, unsigned crm, uint32_t lo, uint32_t hi);
bool coproc_mrrc(cpu_t *c, unsigned cp, unsigned opc1, unsigned crm, uint32_t *lo, uint32_t *hi);
bool coproc_cdp(cpu_t *c, unsigned cp, unsigned opc1, unsigned crd, unsigned crn, unsigned crm, unsigned opc2);
bool dcp_mcrr(cpu_t *c, unsigned opc1, unsigned crm, uint32_t lo, uint32_t hi);
bool dcp_mrrc(cpu_t *c, unsigned opc1, unsigned crm, uint32_t *lo, uint32_t *hi, bool pop);
bool dcp_mrc(cpu_t *c, unsigned crm, unsigned opc2, uint32_t *v, bool pop);
bool dcp_cdp(cpu_t *c, unsigned opc1, unsigned crd, unsigned crn, unsigned crm, unsigned opc2);

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
uint32_t sio_read(cpu_t *c, uint32_t off);
void sio_write(cpu_t *c, uint32_t off, uint32_t v);
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
bool qspi_ss_override(chip_t *m, bool *level);

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
void dma_write(chip_t *m, uint32_t off, uint32_t v);
void dma_kick(chip_t *m);

void pwm_reset(pwm_t *p);
uint32_t pwm_read(chip_t *m, uint32_t off);
void pwm_write(chip_t *m, uint32_t off, uint32_t v);

void qmi_reset(chip_t *m);
uint32_t qmi_read(chip_t *m, uint32_t off);
void qmi_write(chip_t *m, uint32_t off, uint32_t v);
uint32_t xipctrl_read(chip_t *m, uint32_t off);
void xipctrl_write(chip_t *m, uint32_t off, uint32_t v);
void xip_flush(chip_t *m);
void flash_erase(chip_t *m, uint32_t off, uint32_t len);
void flash_program(chip_t *m, uint32_t off, const uint8_t *src, uint32_t len);

void rom_build(chip_t *m);
void rom_hle(cpu_t *c, unsigned n);
void rom_core_start(cpu_t *c);

void devices_spi_frame(chip_t *m, int bus, uint16_t mosi, int bits, uint16_t *miso);
void devices_gpio_changed(chip_t *m, unsigned pin, bool level);
// Clocks one bit into the SPI device wired to these pins, for PIO programs run in bulk.
void devices_spi_bit(chip_t *m, unsigned sck_pin, unsigned mosi_pin, int bit);
int devices_i2c_start(chip_t *m, int bus, uint8_t addr, void **dev);
void devices_i2c_write(void *dev, const uint8_t *buf, int len, bool nostop);
int devices_i2c_read(void *dev, uint8_t *buf, int len, bool nostop);
void devices_uart_tx(chip_t *m, int idx, uint8_t byte);

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
