#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "pcsim/bus.h"
#include "pcsim/devices.h"
#include "pcsim/keys.h"
#include "pcsim/sim.h"

// The STM32F103 on the PicoCalc mainboard: an I2C slave at 0x1F that scans the keyboard into a
// FIFO and reports battery and backlight state. Every exchange is a register byte written
// (bit 7 set for a write, followed by the value), then two bytes read back: [state, key] for
// the FIFO register, [register, value] for the rest, as pcsh's docs/southbridge.md describes.

enum {
    REG_VER = 0x01,
    REG_CFG = 0x02,
    REG_INT = 0x03,
    REG_KEY = 0x04,
    REG_BKL = 0x05,
    REG_DEB = 0x06,
    REG_FRQ = 0x07,
    REG_RST = 0x08,
    REG_FIF = 0x09,
    REG_BK2 = 0x0A,
    REG_BAT = 0x0B,
    REG_OFF = 0x0E,
    REG_COUNT = 0x20,
};

#define REG_WRITE 0x80
#define FIFO_SIZE 31
#define HOLD_MS 300

typedef struct {
    uint8_t state;
    uint8_t key;
} fifo_item_t;

typedef struct {
    uint8_t physical;
    uint8_t reported;
    uint64_t pressed_at_us;
    bool hold_sent;
} held_key_t;

struct southbridge {
    pthread_mutex_t lock;
    uint8_t regs[REG_COUNT];
    fifo_item_t fifo[FIFO_SIZE];
    int fifo_head;
    int fifo_count;
    uint8_t reply[2];
    bool caps_lock;
    bool lshift, rshift;
    held_key_t held[16];
    int nheld;
    uint8_t battery;
    bool charging;
    uint64_t power_off_at_us;
    uint64_t reset_at_us;
    uint32_t generation;
};

// What the keyboard controller sends for a key with Shift held. The keyboard has no keys of its
// own for paging or Home/End/Insert, so the controller reports those for shifted arrows,
// Tab, Del and Enter, and Break for Shift+Esc (see picocalc-z80's DEV_NOTES.md).
static uint8_t shifted(uint8_t key)
{
    if (key >= 'a' && key <= 'z')
        return (uint8_t)(key - 'a' + 'A');
    switch (key) {
    case '1': return '!';
    case '2': return '@';
    case '3': return '#';
    case '4': return '$';
    case '5': return '%';
    case '6': return '^';
    case '7': return '&';
    case '8': return '*';
    case '9': return '(';
    case '0': return ')';
    case '-': return '_';
    case '=': return '+';
    case '[': return '{';
    case ']': return '}';
    case '\\': return '|';
    case ';': return ':';
    case '\'': return '"';
    case ',': return '<';
    case '.': return '>';
    case '/': return '?';
    case '`': return '~';
    case PC_KEY_F1: return PC_KEY_F6;
    case PC_KEY_F2: return PC_KEY_F7;
    case PC_KEY_F3: return PC_KEY_F8;
    case PC_KEY_F4: return PC_KEY_F9;
    case PC_KEY_F5: return PC_KEY_F10;
    case PC_KEY_UP: return PC_KEY_PAGE_UP;
    case PC_KEY_DOWN: return PC_KEY_PAGE_DOWN;
    case PC_KEY_TAB: return PC_KEY_HOME;
    case PC_KEY_DEL: return PC_KEY_END;
    case PC_KEY_ENTER: return PC_KEY_INSERT;
    case PC_KEY_ESC: return PC_KEY_BREAK;
    default: return key;
    }
}

static bool is_modifier(uint8_t key)
{
    return key == PC_KEY_ALT || key == PC_KEY_LSHIFT || key == PC_KEY_RSHIFT || key == PC_KEY_SYM ||
           key == PC_KEY_CTRL;
}

static void push(southbridge_t *sb, uint8_t state, uint8_t key)
{
    if (sb->fifo_count == FIFO_SIZE)
        return;
    sb->fifo[(sb->fifo_head + sb->fifo_count) % FIFO_SIZE] = (fifo_item_t){state, key};
    sb->fifo_count++;
}

static uint8_t translate(const southbridge_t *sb, uint8_t key)
{
    bool shift = sb->lshift || sb->rshift;
    if (key >= 'a' && key <= 'z')
        return (shift != sb->caps_lock) ? shifted(key) : key;
    return shift ? shifted(key) : key;
}

void southbridge_key(southbridge_t *sb, uint8_t key, bool pressed)
{
    pthread_mutex_lock(&sb->lock);
    if (key == PC_KEY_LSHIFT)
        sb->lshift = pressed;
    else if (key == PC_KEY_RSHIFT)
        sb->rshift = pressed;
    else if (key == PC_KEY_CAPS_LOCK && pressed)
        sb->caps_lock = !sb->caps_lock;

    if (pressed) {
        uint8_t reported = is_modifier(key) || key == PC_KEY_CAPS_LOCK ? key : translate(sb, key);
        push(sb, PC_KEY_STATE_PRESSED, reported);
        if (sb->nheld < (int)(sizeof(sb->held) / sizeof(sb->held[0])))
            sb->held[sb->nheld++] = (held_key_t){key, reported, pcsim_uptime_us(), false};
    } else {
        // The release carries the code the press reported, even if Shift changed in between.
        uint8_t reported = key;
        for (int i = 0; i < sb->nheld; i++) {
            if (sb->held[i].physical == key) {
                reported = sb->held[i].reported;
                sb->held[i] = sb->held[--sb->nheld];
                break;
            }
        }
        push(sb, PC_KEY_STATE_RELEASED, reported);
    }
    sb->generation++;
    pthread_mutex_unlock(&sb->lock);
}

// Emits the hold event the controller reports once a key stays down, and runs the delayed
// power-off and reset that BIOS 1.4 offers.
void southbridge_tick(southbridge_t *sb)
{
    uint64_t now = pcsim_uptime_us();
    pthread_mutex_lock(&sb->lock);
    for (int i = 0; i < sb->nheld; i++) {
        held_key_t *h = &sb->held[i];
        if (!h->hold_sent && now - h->pressed_at_us >= HOLD_MS * 1000ull) {
            h->hold_sent = true;
            push(sb, PC_KEY_STATE_HOLD, h->reported);
        }
    }
    bool power_off = sb->power_off_at_us && now >= sb->power_off_at_us;
    bool reset = sb->reset_at_us && now >= sb->reset_at_us;
    if (power_off)
        sb->power_off_at_us = 0;
    if (reset)
        sb->reset_at_us = 0;
    pthread_mutex_unlock(&sb->lock);

    if (reset)
        pcsim_reboot("southbridge reset");
    if (power_off) {
        pthread_mutex_lock(&sb->lock);
        sb->regs[REG_BKL] = 0;
        sb->generation++;
        pthread_mutex_unlock(&sb->lock);
        pcsim_stop(PCSIM_POWERED_OFF, "powered off by the southbridge");
    }
}

static void prepare_reply(southbridge_t *sb, uint8_t reg)
{
    sb->reply[0] = reg;
    switch (reg) {
    case REG_FIF:
        if (sb->fifo_count) {
            fifo_item_t it = sb->fifo[sb->fifo_head];
            sb->fifo_head = (sb->fifo_head + 1) % FIFO_SIZE;
            sb->fifo_count--;
            sb->reply[0] = it.state;
            sb->reply[1] = it.key;
        } else {
            sb->reply[0] = 0;
            sb->reply[1] = 0;
        }
        break;
    case REG_KEY:
        sb->reply[0] = (uint8_t)(sb->fifo_count | (sb->caps_lock ? 0x20 : 0));
        sb->reply[1] = 0;
        break;
    case REG_BAT:
        sb->reply[1] = (uint8_t)(sb->battery | (sb->charging ? 0x80 : 0));
        break;
    default:
        sb->reply[1] = reg < REG_COUNT ? sb->regs[reg] : 0;
        break;
    }
}

static int sb_write(void *ctx, const uint8_t *src, size_t len, bool nostop)
{
    (void)nostop;
    southbridge_t *sb = ctx;
    if (len == 0)
        return 0;
    pthread_mutex_lock(&sb->lock);
    uint8_t reg = src[0] & 0x7f;
    if ((src[0] & REG_WRITE) && len >= 2 && reg < REG_COUNT) {
        uint8_t value = src[1];
        switch (reg) {
        case REG_RST:
            sb->reset_at_us = pcsim_uptime_us() + (uint64_t)(value ? value : 1) * 1000000ull;
            break;
        case REG_OFF:
            sb->power_off_at_us = pcsim_uptime_us() + (uint64_t)(value ? value : 1) * 1000000ull;
            break;
        default:
            sb->regs[reg] = value;
            break;
        }
        sb->generation++;
    }
    prepare_reply(sb, reg);
    pthread_mutex_unlock(&sb->lock);
    return (int)len;
}

static int sb_read(void *ctx, uint8_t *dst, size_t len, bool nostop)
{
    (void)nostop;
    southbridge_t *sb = ctx;
    pthread_mutex_lock(&sb->lock);
    for (size_t i = 0; i < len; i++)
        dst[i] = i < 2 ? sb->reply[i] : 0;
    pthread_mutex_unlock(&sb->lock);
    return (int)len;
}

southbridge_t *southbridge_create(unsigned sda_pin, unsigned scl_pin, uint8_t addr)
{
    southbridge_t *sb = calloc(1, sizeof(*sb));
    pthread_mutex_init(&sb->lock, NULL);
    sb->regs[REG_VER] = 0x14;
    sb->regs[REG_BKL] = 0xff;
    sb->regs[REG_BK2] = 0x00;
    // A nonzero power-off register is how drivers detect BIOS 1.4's power control.
    sb->regs[REG_OFF] = 0x01;
    sb->battery = 85;

    pcsim_i2c_device_t *dev = calloc(1, sizeof(*dev));
    dev->name = "STM32 southbridge";
    dev->sda_pin = sda_pin;
    dev->scl_pin = scl_pin;
    dev->addr = addr;
    dev->write = sb_write;
    dev->read = sb_read;
    dev->ctx = sb;
    pcsim_i2c_attach(dev);
    return sb;
}

void southbridge_destroy(southbridge_t *sb)
{
    pthread_mutex_destroy(&sb->lock);
    free(sb);
}

uint8_t southbridge_lcd_backlight(southbridge_t *sb)
{
    pthread_mutex_lock(&sb->lock);
    uint8_t v = sb->regs[REG_BKL];
    pthread_mutex_unlock(&sb->lock);
    return v;
}

uint8_t southbridge_kbd_backlight(southbridge_t *sb)
{
    pthread_mutex_lock(&sb->lock);
    uint8_t v = sb->regs[REG_BK2];
    pthread_mutex_unlock(&sb->lock);
    return v;
}

bool southbridge_caps_lock(southbridge_t *sb)
{
    pthread_mutex_lock(&sb->lock);
    bool v = sb->caps_lock;
    pthread_mutex_unlock(&sb->lock);
    return v;
}

bool southbridge_shift_held(southbridge_t *sb)
{
    pthread_mutex_lock(&sb->lock);
    bool v = sb->lshift || sb->rshift;
    pthread_mutex_unlock(&sb->lock);
    return v;
}

void southbridge_set_battery(southbridge_t *sb, uint8_t percent, bool charging)
{
    pthread_mutex_lock(&sb->lock);
    sb->battery = percent > 100 ? 100 : percent;
    sb->charging = charging;
    sb->generation++;
    pthread_mutex_unlock(&sb->lock);
}

uint32_t southbridge_generation(southbridge_t *sb)
{
    pthread_mutex_lock(&sb->lock);
    uint32_t g = sb->generation;
    pthread_mutex_unlock(&sb->lock);
    return g;
}
