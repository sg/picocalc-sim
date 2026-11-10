// A check of what arduino-pico firmware such as emu8 relies on, for picocalc-sim's CI: I2C to the
// keyboard controller bit-banged on its pins, as arduino-pico's probe of an address is, and the PIO
// cycle counter arduino-pico runs under FreeRTOS.
#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/i2c.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#define SDA 6
#define SCL 7
#define KBD 0x1f
#define REG_VER 0x01
#define REG_BKL 0x05
#define REG_WRITE 0x80

static int failed;

#define CHECK(ok, ...)                     \
    do {                                   \
        if (!(ok)) {                       \
            printf("ARDUINO FAIL: ");      \
            printf(__VA_ARGS__);           \
            printf("\n");                  \
            failed = 1;                    \
        }                                  \
    } while (0)

// Open drain, as arduino-pico does it: high lets the pull-up have the line, low drives it.
static void line(uint pin, bool high)
{
    gpio_set_dir(pin, high ? GPIO_IN : GPIO_OUT);
    sleep_us(5);
}

static void bb_init(void)
{
    const uint pins[] = {SDA, SCL};
    for (int i = 0; i < 2; i++) {
        gpio_init(pins[i]);
        gpio_pull_up(pins[i]);
        gpio_put(pins[i], 0);
        gpio_set_dir(pins[i], GPIO_IN);
    }
    sleep_us(20);
}

static void start(void)
{
    line(SDA, 1);
    line(SCL, 1);
    line(SDA, 0);
    line(SCL, 0);
}

static void stop(void)
{
    line(SDA, 0);
    line(SCL, 1);
    line(SDA, 1);
}

static bool send(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        line(SDA, (b >> i) & 1);
        line(SCL, 1);
        line(SCL, 0);
    }
    line(SDA, 1);
    line(SCL, 1);
    bool ack = !gpio_get(SDA);
    line(SCL, 0);
    return ack;
}

static uint8_t recv(bool ack)
{
    uint8_t b = 0;
    line(SDA, 1);
    for (int i = 0; i < 8; i++) {
        line(SCL, 1);
        b = (uint8_t)(b << 1 | gpio_get(SDA));
        line(SCL, 0);
    }
    line(SDA, !ack);
    line(SCL, 1);
    line(SCL, 0);
    line(SDA, 1);
    return b;
}

static bool probe(uint8_t addr)
{
    start();
    bool ack = send((uint8_t)(addr << 1));
    stop();
    return ack;
}

static uint32_t read_x(PIO pio, uint sm)
{
    pio_sm_exec(pio, sm, pio_encode_mov(pio_isr, pio_x));
    pio_sm_exec(pio, sm, pio_encode_push(false, false));
    return pio_sm_get_blocking(pio, sm);
}

// arduino-pico's ccount program, here with a delay or a fractional divider as well.
static void counter(unsigned delay, float div)
{
    PIO pio = pio0;
    uint sm = 0;
    uint16_t ins[] = {(uint16_t)(pio_encode_jmp_x_dec(0) | pio_encode_delay(delay)), (uint16_t)pio_encode_jmp_y_dec(0)};
    struct pio_program prog = {.instructions = ins, .length = 2, .origin = 0};
    pio_clear_instruction_memory(pio);
    pio_add_program(pio, &prog);
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, 0, 1);
    sm_config_set_clkdiv(&c, div);
    pio_sm_init(pio, sm, 0, &c);
    pio_sm_exec(pio, sm, pio_encode_mov_not(pio_x, pio_null));
    pio_sm_exec(pio, sm, pio_encode_mov_not(pio_y, pio_null));
    pio_sm_set_enabled(pio, sm, true);
    uint32_t a = read_x(pio, sm);
    uint64_t t0 = time_us_64();
    sleep_ms(100);
    uint32_t b = read_x(pio, sm);
    uint64_t us = time_us_64() - t0;
    pio_sm_set_enabled(pio, sm, false);
    double want = (double)clock_get_hz(clk_sys) * (double)us / 1e6 / div / (delay + 1);
    double got = (double)(uint32_t)(a - b);
    // The reads take a few cycles, and picocalc-sim rounds a system clock cycle to whole picoseconds.
    CHECK(got > want * 0.999 - 200 && got < want * 1.001 + 200,
          "PIO counter with delay %u and divider %.2f counted %.0f in %llu us, expected %.0f", delay, div, got,
          (unsigned long long)us, want);
}

int main(void)
{
    stdio_init_all();
    bb_init();
    CHECK(probe(KBD), "no ACK from the keyboard controller at 0x%02x", KBD);
    CHECK(!probe(0x2a), "an ACK from 0x2a, where nothing is");

    start();
    bool ok = send(KBD << 1) && send(REG_BKL | REG_WRITE) && send(0x42);
    stop();
    CHECK(ok, "bit-banged write not acknowledged");

    start();
    ok = send(KBD << 1) && send(REG_VER);
    stop();
    start();
    ok = ok && send(KBD << 1 | 1);
    uint8_t r0 = recv(true), r1 = recv(false);
    stop();
    CHECK(ok && r0 == REG_VER, "bit-banged read of the version gave %02x %02x (acknowledged: %d)", r0, r1, ok);

    i2c_init(i2c1, 100000);
    gpio_set_function(SDA, GPIO_FUNC_I2C);
    gpio_set_function(SCL, GPIO_FUNC_I2C);
    uint8_t reg = REG_VER, hw[2] = {0};
    i2c_write_blocking(i2c1, KBD, &reg, 1, false);
    i2c_read_blocking(i2c1, KBD, hw, 2, false);
    CHECK(hw[0] == REG_VER && hw[1] == r1, "the I2C block read the version as %02x %02x, bit-banging as %02x %02x",
          hw[0], hw[1], r0, r1);
    reg = REG_BKL;
    i2c_write_blocking(i2c1, KBD, &reg, 1, false);
    i2c_read_blocking(i2c1, KBD, hw, 2, false);
    CHECK(hw[0] == REG_BKL && hw[1] == 0x42, "the backlight read back as %02x %02x after a bit-banged write of 42",
          hw[0], hw[1]);

    counter(0, 1.0f);
    counter(3, 1.0f);
    counter(0, 2.5f);
    if (!failed)
        printf("ARDUINO PASS: bit-banged I2C and the PIO cycle counter\n");
    for (;;)
        tight_loop_contents();
}
