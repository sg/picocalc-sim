#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chip.h"
#include "picocalc-sim/sim.h"

// A computer on the far end of the board's USB cable. When a device pulls D+ up it waits for the
// connection to settle, resets the port and enumerates the device much as Linux does, then logs
// what it found and keeps the bus awake with start-of-frame packets. Each transaction takes the bus
// time a full-speed packet would, and one the device NAKs is tried again a little later, less often
// the longer the device has nothing.

enum { H_DETACHED, H_DEBOUNCE, H_RESET, H_RECOVERY, H_CONTROL, H_CONFIGURED, H_FAILED };
enum { CT_SETUP, CT_DATA, CT_STATUS };
enum { R_OK, R_STALL, R_ERROR };

// What the host asks for, in order; the steps past STRINGS run only for the strings the device has.
enum {
    S_FIRST_DESC,
    S_SET_ADDRESS,
    S_DEVICE_DESC,
    S_CONFIG_HEAD,
    S_CONFIG,
    S_LANGIDS,
    S_MANUFACTURER,
    S_PRODUCT,
    S_SERIAL,
    S_SET_CONFIG,
};

#define MAX_DATA 4096

typedef struct usb_host {
    chip_t *m;
    emu_event_t ev, sof_ev;
    int state, step, attempts;
    unsigned frame, addr, mps0;
    bool debug;
    // The control transfer in progress.
    uint8_t setup[8];
    uint8_t data[MAX_DATA];
    unsigned want, got;
    int stage, errors;
    unsigned toggle, naks;
    uint64_t deadline;
    uint8_t dev[18];
    unsigned cfg_len;
    uint8_t cfg[MAX_DATA];
    uint16_t langid;
    char str[3][128];
} usb_host_t;

static void hlog(usb_host_t *h, const char *fmt, ...) __attribute__((format(PICOCALC_SIM_PRINTF_FORMAT, 2, 3)));

static void hlog(usb_host_t *h, const char *fmt, ...)
{
    (void)h;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    picocalc_sim_log("usb: %s", buf);
}

// A transaction carrying n data bytes: token, data and handshake packets with their sync fields,
// CRCs and gaps, at 12 Mb/s with a sixth more for bit stuffing.
static uint64_t bus_time(unsigned n)
{
    uint64_t bits = (uint64_t)(n + 12) * 8 * 7 / 6 + 40;
    return bits * PS_PER_US / 12;
}

static uint64_t nak_delay(usb_host_t *h)
{
    if (h->naks < 8)
        return bus_time(0);
    if (h->naks < 100)
        return 125 * PS_PER_US;
    return PS_PER_MS;
}

static void sof_tick(chip_t *m, emu_event_t *ev)
{
    usb_host_t *h = ev->ctx;
    h->frame = (h->frame + 1) & 0x7ff;
    usb_bus_sof(m, h->frame);
    ev_schedule(m, ev, m->now + PS_PER_MS);
}

static void stop_bus(usb_host_t *h)
{
    ev_cancel(h->m, &h->ev);
    ev_cancel(h->m, &h->sof_ev);
}

static void start_reset(usb_host_t *h)
{
    chip_t *m = h->m;
    ev_cancel(m, &h->sof_ev);
    usb_bus_reset(m, true);
    h->state = H_RESET;
    h->addr = 0;
    h->mps0 = 64;
    ev_schedule(m, &h->ev, m->now + 20 * PS_PER_MS);
}

static void control(usb_host_t *h, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len)
{
    chip_t *m = h->m;
    uint8_t s[8] = {type, req, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)index, (uint8_t)(index >> 8),
                    (uint8_t)len, (uint8_t)(len >> 8)};
    memcpy(h->setup, s, 8);
    h->want = len > MAX_DATA ? MAX_DATA : len;
    h->got = 0;
    h->stage = CT_SETUP;
    h->errors = 0;
    h->naks = 0;
    h->deadline = m->now + 5 * PS_PER_S;
    h->state = H_CONTROL;
    if (h->debug)
        hlog(h, "addr %u setup %02x %02x %04x %04x %04x", h->addr, type, req, value, index, len);
    ev_schedule(m, &h->ev, m->now + bus_time(0));
}

static void get_descriptor(usb_host_t *h, unsigned type, unsigned index, uint16_t lang, uint16_t len)
{
    control(h, 0x80, 6, (uint16_t)(type << 8 | index), lang, len);
}

static void ascii(char *out, size_t cap, const uint8_t *d, unsigned n)
{
    size_t o = 0;
    for (unsigned i = 2; i + 1 < n && i < d[0] && o + 1 < cap; i += 2) {
        unsigned c = d[i] | d[i + 1] << 8;
        out[o++] = c >= 0x20 && c < 0x7f ? (char)c : '?';
    }
    out[o] = 0;
}

static const char *class_name(unsigned c)
{
    switch (c) {
    case 0x01: return "audio";
    case 0x02: return "CDC control";
    case 0x03: return "HID";
    case 0x07: return "printer";
    case 0x08: return "mass storage";
    case 0x0a: return "CDC data";
    case 0x0e: return "video";
    case 0xe0: return "wireless";
    case 0xef: return "miscellaneous";
    case 0xfe: return "application-specific";
    case 0xff: return "vendor-specific";
    default: return "other";
    }
}

static void report(usb_host_t *h)
{
    char ifs[256] = "";
    size_t o = 0;
    for (unsigned i = 0; i + 2 <= h->cfg_len && h->cfg[i] >= 2; i += h->cfg[i]) {
        const uint8_t *d = h->cfg + i;
        if (d[1] == 4 && d[0] >= 9 && d[3] == 0 && o < sizeof(ifs))
            o += (size_t)snprintf(ifs + o, sizeof(ifs) - o, "%s%u %s", o ? ", " : "", d[2], class_name(d[5]));
    }
    hlog(h, "%04x:%04x \"%s\" from \"%s\", serial \"%s\", USB %x.%02x; interfaces %s",
         h->dev[8] | h->dev[9] << 8, h->dev[10] | h->dev[11] << 8, h->str[1], h->str[0], h->str[2], h->dev[3],
         h->dev[2], ifs[0] ? ifs : "none");
}

static void fail(usb_host_t *h, const char *what)
{
    if (++h->attempts < 3) {
        if (h->debug)
            hlog(h, "%s failed; resetting the port", what);
        h->step = S_FIRST_DESC;
        start_reset(h);
        return;
    }
    hlog(h, "enumeration failed: %s; port disabled", what);
    stop_bus(h);
    h->state = H_FAILED;
}

// Asks for the next thing enumeration needs, or finishes.
static void next_step(usb_host_t *h)
{
    static const unsigned str_index[3] = {14, 15, 16};
    for (;;) {
        switch (h->step) {
        case S_FIRST_DESC: get_descriptor(h, 1, 0, 0, 64); return;
        case S_SET_ADDRESS: control(h, 0x00, 5, 1, 0, 0); return;
        case S_DEVICE_DESC: get_descriptor(h, 1, 0, 0, 18); return;
        case S_CONFIG_HEAD: get_descriptor(h, 2, 0, 0, 9); return;
        case S_CONFIG: get_descriptor(h, 2, 0, 0, (uint16_t)(h->cfg[2] | h->cfg[3] << 8)); return;
        case S_LANGIDS:
            if (!h->dev[14] && !h->dev[15] && !h->dev[16]) {
                h->step = S_SET_CONFIG;
                continue;
            }
            get_descriptor(h, 3, 0, 0, 255);
            return;
        case S_MANUFACTURER:
        case S_PRODUCT:
        case S_SERIAL: {
            unsigned idx = h->dev[str_index[h->step - S_MANUFACTURER]];
            if (!idx) {
                h->step++;
                continue;
            }
            get_descriptor(h, 3, idx, h->langid, 255);
            return;
        }
        case S_SET_CONFIG: control(h, 0x00, 9, h->cfg[5], 0, 0); return;
        default: return;
        }
    }
}

static void step_done(usb_host_t *h, int r)
{
    chip_t *m = h->m;
    static const char *const names[] = {
        "GET_DESCRIPTOR(device, 64)", "SET_ADDRESS", "GET_DESCRIPTOR(device)", "GET_DESCRIPTOR(configuration, 9)",
        "GET_DESCRIPTOR(configuration)", "GET_DESCRIPTOR(string 0)", "GET_DESCRIPTOR(manufacturer)",
        "GET_DESCRIPTOR(product)", "GET_DESCRIPTOR(serial)", "SET_CONFIGURATION",
    };
    if (h->debug)
        hlog(h, "%s: %s, %u bytes", names[h->step], r == R_OK ? "ok" : r == R_STALL ? "stalled" : "error", h->got);
    bool string = h->step >= S_MANUFACTURER && h->step <= S_SERIAL;
    if (r != R_OK && !(string && r == R_STALL)) {
        // A device without string descriptors may stall the request for their languages.
        if (h->step == S_LANGIDS && r == R_STALL) {
            h->step = S_SET_CONFIG;
            next_step(h);
            return;
        }
        fail(h, names[h->step]);
        return;
    }
    switch (h->step) {
    case S_FIRST_DESC: {
        unsigned mps = h->got >= 8 ? h->data[7] : 0;
        if (mps != 8 && mps != 16 && mps != 32 && mps != 64) {
            fail(h, "a device descriptor with a bad EP0 packet size");
            return;
        }
        // Like Linux, reset the port again before giving the device its address.
        h->step = S_SET_ADDRESS;
        start_reset(h);
        h->mps0 = mps;
        return;
    }
    case S_SET_ADDRESS:
        h->addr = 1;
        h->step = S_DEVICE_DESC;
        // The device has 2 ms to take its new address.
        h->state = H_RECOVERY;
        ev_schedule(m, &h->ev, m->now + 2 * PS_PER_MS);
        return;
    case S_DEVICE_DESC:
        if (h->got < 18 || h->data[1] != 1) {
            fail(h, "a short device descriptor");
            return;
        }
        memcpy(h->dev, h->data, 18);
        h->mps0 = h->dev[7];
        break;
    case S_CONFIG_HEAD:
        if (h->got < 9 || h->data[1] != 2) {
            fail(h, "a short configuration descriptor");
            return;
        }
        memcpy(h->cfg, h->data, 9);
        break;
    case S_CONFIG:
        memcpy(h->cfg, h->data, h->got);
        h->cfg_len = h->got;
        break;
    case S_LANGIDS: h->langid = h->got >= 4 ? (uint16_t)(h->data[2] | h->data[3] << 8) : 0x0409; break;
    case S_MANUFACTURER:
    case S_PRODUCT:
    case S_SERIAL:
        if (r == R_OK)
            ascii(h->str[h->step - S_MANUFACTURER], sizeof(h->str[0]), h->data, h->got);
        break;
    case S_SET_CONFIG:
        h->state = H_CONFIGURED;
        report(h);
        return;
    default: break;
    }
    h->step++;
    next_step(h);
}

// One transaction of the control transfer, then the next after the bus time it took.
static void control_step(usb_host_t *h)
{
    chip_t *m = h->m;
    uint8_t buf[1023];
    unsigned n = 0, pid = 0;
    bool in_data = h->setup[0] & 0x80;
    int r;
    uint64_t t = bus_time(0);
    if (m->now > h->deadline) {
        step_done(h, R_ERROR);
        return;
    }
    switch (h->stage) {
    case CT_SETUP:
        r = usb_bus_setup(m, h->addr, h->setup);
        t = bus_time(8);
        if (r == USB_ACK) {
            h->toggle = 1;
            h->stage = h->want ? CT_DATA : CT_STATUS;
        }
        break;
    case CT_DATA:
        if (in_data) {
            r = usb_bus_in(m, h->addr, 0, buf, &n, &pid);
            t = bus_time(n);
            if (r != USB_ACK)
                break;
            if (n > h->mps0) {
                step_done(h, R_ERROR);
                return;
            }
            // A packet with the toggle already seen repeats one whose ACK went astray.
            if (pid != h->toggle)
                break;
            h->toggle ^= 1;
            unsigned take = n < h->want - h->got ? n : h->want - h->got;
            memcpy(h->data + h->got, buf, take);
            h->got += take;
            if (n < h->mps0 || h->got >= h->want)
                h->stage = CT_STATUS;
        } else {
            n = h->want - h->got < h->mps0 ? h->want - h->got : h->mps0;
            r = usb_bus_out(m, h->addr, 0, h->toggle, h->data + h->got, n);
            t = bus_time(n);
            if (r == USB_ACK) {
                h->got += n;
                h->toggle ^= 1;
                if (h->got >= h->want)
                    h->stage = CT_STATUS;
            }
        }
        break;
    default:
        // The status stage goes the other way from the data, as a zero-length DATA1 packet.
        if (in_data && h->want) {
            r = usb_bus_out(m, h->addr, 0, 1, NULL, 0);
        } else {
            r = usb_bus_in(m, h->addr, 0, buf, &n, &pid);
            if (r == USB_ACK && n) {
                step_done(h, R_ERROR);
                return;
            }
        }
        if (r == USB_ACK) {
            h->state = H_RECOVERY;
            step_done(h, R_OK);
            return;
        }
        break;
    }
    switch (r) {
    case USB_ACK: h->naks = 0; break;
    case USB_NAK:
        h->naks++;
        t = nak_delay(h);
        break;
    case USB_STALL: step_done(h, R_STALL); return;
    default:
        if (++h->errors >= 3) {
            step_done(h, R_ERROR);
            return;
        }
        break;
    }
    ev_schedule(m, &h->ev, m->now + t);
}

static void host_event(chip_t *m, emu_event_t *ev)
{
    usb_host_t *h = ev->ctx;
    switch (h->state) {
    case H_DEBOUNCE: start_reset(h); break;
    case H_RESET:
        usb_bus_reset(m, false);
        ev_schedule(m, &h->sof_ev, m->now + PS_PER_MS);
        // The device has 10 ms to recover from the reset.
        h->state = H_RECOVERY;
        ev_schedule(m, ev, m->now + 10 * PS_PER_MS);
        break;
    case H_RECOVERY: next_step(h); break;
    case H_CONTROL: control_step(h); break;
    default: break;
    }
}

void usbhost_pullup(chip_t *m, bool on)
{
    usb_host_t *h = m->usb.host;
    if (!h)
        return;
    if (on) {
        if (h->state != H_DETACHED)
            return;
        if (h->debug)
            hlog(h, "device connected");
        h->state = H_DEBOUNCE;
        h->step = S_FIRST_DESC;
        h->attempts = 0;
        memset(h->str, 0, sizeof(h->str));
        // The connection settles for 100 ms before the port is reset.
        ev_schedule(m, &h->ev, m->now + 100 * PS_PER_MS);
        return;
    }
    if (h->state == H_DETACHED)
        return;
    if (h->state == H_CONFIGURED || h->debug)
        hlog(h, "device disconnected");
    stop_bus(h);
    if (h->state == H_RESET)
        usb_bus_reset(m, false);
    h->state = H_DETACHED;
}

void usbhost_init(chip_t *m)
{
    usb_host_t *h = calloc(1, sizeof(*h));
    h->m = m;
    h->debug = getenv("PICOCALC_SIM_USB_DEBUG") != NULL;
    h->ev.fn = host_event;
    h->ev.ctx = h;
    h->sof_ev.fn = sof_tick;
    h->sof_ev.ctx = h;
    m->usb.host = h;
    usb_bus_attach(m, true);
    // The cable brings VBUS, which the boards without a radio sense on GPIO24.
    if (!m->board->wifi) {
        m->gpio[24].ext_driven = true;
        m->gpio[24].ext_level = true;
    }
}

void usbhost_free(chip_t *m)
{
    usb_host_t *h = m->usb.host;
    if (!h)
        return;
    stop_bus(h);
    usb_bus_attach(m, false);
    if (!m->board->wifi)
        m->gpio[24].ext_driven = false;
    m->usb.host = NULL;
    free(h);
}
