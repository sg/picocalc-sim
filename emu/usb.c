#include <string.h>

#include "chip.h"

// The USB controller as a full-speed device: its registers, the endpoint and buffer control words
// it shares with the CPU in DPRAM, and the transactions a host on the far end of the cable makes,
// each of which happens at once. Host mode keeps its registers but never moves anything.

enum {
    MAIN_CONTROLLER_EN = 1u << 0,
    MAIN_HOST_NDEVICE = 1u << 1,
    MAIN_PHY_ISO = 1u << 2,

    SIE_START_TRANS = 1u << 0,
    SIE_STOP_TRANS = 1u << 4,
    SIE_RESUME = 1u << 12,
    SIE_RESET_BUS = 1u << 13,
    SIE_PULLUP_EN = 1u << 16,
    SIE_TRANSCEIVER_PD = 1u << 18,
    SIE_EP0_STOP_ON_SHORT = 1u << 19,
    SIE_EP0_INT_NAK = 1u << 27,
    SIE_EP0_INT_2BUF = 1u << 28,
    SIE_EP0_INT_1BUF = 1u << 29,
    SIE_EP0_DOUBLE_BUF = 1u << 30,
    SIE_EP0_INT_STALL = 1u << 31,
    SIE_SELF_CLEARING = SIE_START_TRANS | SIE_STOP_TRANS | SIE_RESUME | SIE_RESET_BUS,

    ST_VBUS_DETECTED = 1u << 0,
    ST_SUSPENDED = 1u << 4,
    ST_RESUME = 1u << 11,
    ST_RX_SHORT_PACKET = 1u << 12,
    ST_CONNECTED = 1u << 16,
    ST_SETUP_REC = 1u << 17,
    ST_TRANS_COMPLETE = 1u << 18,
    ST_BUS_RESET = 1u << 19,
    ST_ENDPOINT_ERROR = 1u << 23,
    ST_CRC_ERROR = 1u << 24,
    ST_BIT_STUFF_ERROR = 1u << 25,
    ST_RX_OVERFLOW = 1u << 26,
    ST_RX_TIMEOUT = 1u << 27,
    ST_STALL_REC = 1u << 29,
    ST_DATA_SEQ_ERROR = 1u << 31,

    MUX_TO_PHY = 1u << 0,

    PWR_VBUS_DETECT = 1u << 2,
    PWR_VBUS_DETECT_OVERRIDE_EN = 1u << 3,

    BC_LEN = 0x3ff,
    BC_AVAILABLE = 1u << 10,
    BC_STALL = 1u << 11,
    BC_RESET_SEL = 1u << 12,
    BC_PID = 1u << 13,
    BC_LAST = 1u << 14,
    BC_FULL = 1u << 15,

    EP_TYPE_ISO = 1,
};

enum { LINE_SE0, LINE_J, LINE_K };

#define SUSPEND_PS (3 * PS_PER_MS)

static bool phy_on(chip_t *m)
{
    usb_t *u = &m->usb;
    return !(m->resets & (1u << 28)) && (m->rp2040 || !(u->main_ctrl & MAIN_PHY_ISO)) && (u->muxing & MUX_TO_PHY) &&
           !(u->sie_ctrl & SIE_TRANSCEIVER_PD);
}

// The full-speed pull-up on D+, which is how a host sees the device.
static bool pullup(chip_t *m)
{
    usb_t *u = &m->usb;
    return phy_on(m) && (u->sie_ctrl & SIE_PULLUP_EN) && !(u->main_ctrl & MAIN_HOST_NDEVICE);
}

// clk_usb has to be 48 MHz to within what full speed tolerates for the controller to follow the bus.
static bool listening(chip_t *m)
{
    usb_t *u = &m->usb;
    return phy_on(m) && (u->main_ctrl & (MAIN_CONTROLLER_EN | MAIN_HOST_NDEVICE)) == MAIN_CONTROLLER_EN &&
           m->usb_hz >= 47880000u && m->usb_hz <= 48120000u;
}

// None of the boards wire VBUS to the controller's own detect input, so only the override sets it.
static bool vbus_detected(chip_t *m)
{
    uint32_t p = m->usb.pwr;
    return (p & PWR_VBUS_DETECT_OVERRIDE_EN) && (p & PWR_VBUS_DETECT);
}

// Without a host, D+ is pulled up or else down to SE0 by the PHY's own pull-downs.
static unsigned line_state(chip_t *m)
{
    usb_t *u = &m->usb;
    if (u->host_present && u->host_reset)
        return LINE_SE0;
    return u->pullup ? LINE_J : LINE_SE0;
}

static uint32_t sie_status(chip_t *m)
{
    usb_t *u = &m->usb;
    uint32_t s = u->sie_status;
    if (vbus_detected(m))
        s |= ST_VBUS_DETECTED;
    if (phy_on(m))
        s |= (uint32_t)u->line << 2;
    if (u->suspended)
        s |= ST_SUSPENDED;
    if (u->connected)
        s |= ST_CONNECTED;
    return s;
}

static uint32_t intr(chip_t *m)
{
    usb_t *u = &m->usb;
    uint32_t s = sie_status(m), r = 0;
    bool host = u->main_ctrl & MAIN_HOST_NDEVICE;
    static const struct {
        uint32_t st;
        uint8_t bit;
    } from_status[] = {
        {ST_TRANS_COMPLETE, 3}, {ST_DATA_SEQ_ERROR, 5}, {ST_RX_TIMEOUT, 6}, {ST_RX_OVERFLOW, 7},
        {ST_BIT_STUFF_ERROR, 8}, {ST_CRC_ERROR, 9},     {ST_STALL_REC, 10}, {ST_VBUS_DETECTED, 11},
        {ST_BUS_RESET, 12},      {ST_SETUP_REC, 16},
    };
    for (size_t i = 0; i < sizeof(from_status) / sizeof(from_status[0]); i++)
        if (s & from_status[i].st)
            r |= 1u << from_status[i].bit;
    if (u->buff_status)
        r |= 1u << 4;
    if (u->conn_irq)
        r |= 1u << 13;
    if (u->susp_irq)
        r |= 1u << 14;
    if (s & ST_RESUME)
        r |= host ? 1u << 1 : 1u << 15;
    if (u->sof_irq)
        r |= host ? 1u << 2 : 1u << 17;
    if (u->ep_abort_done)
        r |= 1u << 18;
    if (u->stall_nak)
        r |= 1u << 19;
    if (!m->rp2040) {
        if (s & ST_RX_SHORT_PACKET)
            r |= 1u << 20;
        if (s & ST_ENDPOINT_ERROR)
            r |= 1u << 21;
        if (u->sm_watchdog & (1u << 20))
            r |= 1u << 22;
        if (u->nak_poll & (1u << 27))
            r |= 1u << 23;
    }
    return r;
}

static void irq_update(chip_t *m)
{
    usb_t *u = &m->usb;
    chip_irq(m, IRQ_USBCTRL, ((intr(m) | u->intf) & u->inte) != 0);
}

static void set_suspended(chip_t *m, bool on)
{
    usb_t *u = &m->usb;
    if (u->suspended == on)
        return;
    u->suspended = on;
    u->susp_irq = true;
    if (!on)
        u->sie_status |= ST_RESUME;
}

static void idle_check(chip_t *m, emu_event_t *ev)
{
    usb_t *u = &m->usb;
    if (!u->listening || u->line != LINE_J || u->suspended)
        return;
    if (m->now >= u->activity_t + SUSPEND_PS) {
        set_suspended(m, true);
        irq_update(m);
    } else {
        ev_schedule(m, ev, u->activity_t + SUSPEND_PS);
    }
}

// Bus traffic wakes a suspended device and holds off its next suspend.
static void activity(chip_t *m)
{
    usb_t *u = &m->usb;
    u->activity_t = m->now;
    set_suspended(m, false);
}

// Follows the pull-up, line state and connection after anything that can change them.
static void update(chip_t *m)
{
    usb_t *u = &m->usb;
    bool was_listening = u->listening;
    unsigned was_line = u->line;
    u->listening = listening(m);
    bool pu = pullup(m);
    bool pu_changed = pu != u->pullup;
    u->pullup = pu;
    u->line = line_state(m);
    bool conn = u->listening && pu && vbus_detected(m);
    if (conn != u->connected) {
        u->connected = conn;
        u->conn_irq = true;
    }
    if (!u->listening) {
        u->suspended = false;
        ev_cancel(m, &u->idle_ev);
    } else if (u->line == LINE_SE0) {
        if (!was_listening || was_line != LINE_SE0) {
            u->sie_status |= ST_BUS_RESET;
            activity(m);
        }
        ev_cancel(m, &u->idle_ev);
    } else if (!u->suspended) {
        if (!was_listening || was_line != LINE_J)
            u->activity_t = m->now;
        u->idle_ev.fn = idle_check;
        if (!u->idle_ev.queued)
            ev_schedule(m, &u->idle_ev, u->activity_t + SUSPEND_PS);
    }
    irq_update(m);
    if (pu_changed && u->host_present)
        usbhost_pullup(m, pu);
}

void usb_clocks_changed(chip_t *m)
{
    update(m);
}

void usb_reset(chip_t *m)
{
    usb_t *u = &m->usb;
    ev_cancel(m, &u->idle_ev);
    bool host_present = u->host_present, host_reset = u->host_reset, was_pullup = u->pullup;
    struct usb_host *host = u->host;
    memset(u, 0, sizeof(*u));
    u->idle_ev.fn = idle_check;
    u->host_present = host_present;
    u->host_reset = host_reset;
    u->host = host;
    u->pullup = was_pullup;
    if (host)
        usbhost_chip_reset(m);
    u->nak_poll = 0x00100010u;
    u->phy_trim = 0x1f1fu;
    if (!m->rp2040) {
        u->main_ctrl = MAIN_PHY_ISO;
        u->sie_ctrl = 1u << 15;
        u->muxing = MUX_TO_PHY;
        u->linestate_tuning = 0xf8;
    }
    update(m);
}

uint32_t usb_read(chip_t *m, uint32_t off, bool peek)
{
    usb_t *u = &m->usb;
    if (off < 0x40)
        return u->addr_endp[off >> 2];
    switch (off) {
    case 0x40: return u->main_ctrl;
    case 0x48:
        if (!peek && u->sof_irq) {
            u->sof_irq = false;
            irq_update(m);
        }
        return u->sof_rd;
    case 0x4c: return u->sie_ctrl;
    case 0x50: return sie_status(m);
    case 0x54: return u->int_ep_ctrl;
    case 0x58: return u->buff_status;
    case 0x5c: return u->buff_which;
    case 0x60: return u->ep_abort;
    case 0x64: return u->ep_abort_done;
    case 0x68: return u->ep_stall_arm;
    case 0x6c: return u->nak_poll;
    case 0x70: return u->stall_nak;
    case 0x74: return u->muxing;
    case 0x78: return u->pwr;
    case 0x7c: return u->phy_direct;
    case 0x80: return u->phy_override;
    case 0x84: return u->phy_trim;
    case 0x8c: return intr(m);
    case 0x90: return u->inte;
    case 0x94: return u->intf;
    case 0x98: return (intr(m) | u->intf) & u->inte;
    default: break;
    }
    if (m->rp2040)
        return 0;
    switch (off) {
    case 0x88: return u->linestate_tuning;
    // The PHY's 48 MHz count, free-running and when the last SOF came.
    case 0x100: return (uint32_t)(m->now / PS_PER_NS * 48 / 1000) & 0x1fffff;
    case 0x104: return u->sof_ts_last;
    case 0x10c: return u->ep_tx_error;
    case 0x110: return u->ep_rx_error;
    case 0x114: return u->sm_watchdog;
    default: return 0;
    }
}

// The set, clear and XOR aliases act on the read-write fields; a one written through any of them
// clears a write-one-to-clear bit, as a plain write does.
static uint32_t aliased(uint32_t old, uint32_t v, unsigned alias)
{
    switch (alias) {
    case 1: return old ^ v;
    case 2: return old | v;
    case 3: return old & ~v;
    default: return v;
    }
}

// A finished buffer's interrupt, queued behind one already flagged for the same endpoint.
static void flag_buffer(chip_t *m, unsigned bit, unsigned which)
{
    usb_t *u = &m->usb;
    uint32_t b = 1u << bit;
    if (u->buff_status & b) {
        u->queued |= b;
        u->queued_which = (u->queued_which & ~b) | (which ? b : 0);
    } else {
        u->buff_status |= b;
        u->buff_which = (u->buff_which & ~b) | (which ? b : 0);
    }
}

static void clear_buff_status(chip_t *m, uint32_t v)
{
    usb_t *u = &m->usb;
    u->buff_status &= ~v;
    uint32_t again = v & u->queued;
    u->queued &= ~again;
    u->buff_status |= again;
    u->buff_which = (u->buff_which & ~again) | (u->queued_which & again);
}

void usb_write(chip_t *m, uint32_t off, uint32_t v, unsigned alias)
{
    usb_t *u = &m->usb;
    if (off < 0x40) {
        uint32_t *r = &u->addr_endp[off >> 2];
        *r = aliased(*r, v, alias) & (off ? 0x060f007fu : 0x000f007fu);
        return;
    }
    bool a = !m->rp2040;
    switch (off) {
    case 0x40: u->main_ctrl = aliased(u->main_ctrl, v, alias) & (a ? 0x80000007u : 0x80000003u); break;
    case 0x44: u->sof_wr = aliased(u->sof_wr, v, alias) & 0x7ff; break;
    case 0x4c: {
        uint32_t nv = aliased(u->sie_ctrl, v, alias) & (a ? 0xff0fbf5fu : 0xff07bf5fu);
        // Remote wakeup: the device drives resume signalling, which the host answers by resuming
        // the bus.
        if ((nv & SIE_RESUME) && u->suspended && u->listening) {
            set_suspended(m, false);
            u->sie_status &= ~ST_RESUME;
        }
        u->sie_ctrl = nv & ~SIE_SELF_CLEARING;
        break;
    }
    case 0x50: {
        uint32_t wc = a ? 0xff8e1800u : 0xff0f0b10u;
        u->sie_status &= ~(v & wc);
        if (v & ST_CONNECTED)
            u->conn_irq = false;
        if (v & ST_SUSPENDED)
            u->susp_irq = false;
        break;
    }
    case 0x54: u->int_ep_ctrl = aliased(u->int_ep_ctrl, v, alias) & 0xfffe; break;
    case 0x58: clear_buff_status(m, v); break;
    case 0x60:
        u->ep_abort = aliased(u->ep_abort, v, alias);
        // Transactions are whole, so an endpoint being aborted is always idle.
        u->ep_abort_done |= u->ep_abort;
        break;
    case 0x64: u->ep_abort_done = (u->ep_abort_done & ~v) | u->ep_abort; break;
    case 0x68: u->ep_stall_arm = aliased(u->ep_stall_arm, v, alias) & 3; break;
    case 0x6c:
        if (a) {
            uint32_t rw = 0x07ff03ffu & ~(1u << 27);
            u->nak_poll = (aliased(u->nak_poll, v, alias) & rw) | (u->nak_poll & ~rw & ~(v & (1u << 27)));
        } else {
            u->nak_poll = aliased(u->nak_poll, v, alias) & 0x03ff03ffu;
        }
        break;
    case 0x70: u->stall_nak &= ~v; break;
    case 0x74: u->muxing = aliased(u->muxing, v, alias) & (a ? 0x8000001fu : 0xfu); break;
    case 0x78: u->pwr = aliased(u->pwr, v, alias) & 0x3f; break;
    case 0x7c: u->phy_direct = aliased(u->phy_direct, v, alias) & (a ? 0x03ffff77u : 0x007fff77u); break;
    case 0x80: u->phy_override = aliased(u->phy_override, v, alias) & (a ? 0x00079fffu : 0x00009fffu); break;
    case 0x84: u->phy_trim = aliased(u->phy_trim, v, alias) & 0x1f1f; break;
    case 0x88:
        if (a)
            u->linestate_tuning = aliased(u->linestate_tuning, v, alias) & 0xfff;
        break;
    case 0x90: u->inte = aliased(u->inte, v, alias) & (a ? 0xffffffu : 0xfffffu); break;
    case 0x94: u->intf = aliased(u->intf, v, alias) & (a ? 0xffffffu : 0xfffffu); break;
    case 0x10c:
        if (a)
            u->ep_tx_error &= ~v;
        break;
    case 0x110:
        if (a)
            u->ep_rx_error &= ~v;
        break;
    case 0x114:
        if (a) {
            uint32_t rw = 0xfffffu;
            u->sm_watchdog = (aliased(u->sm_watchdog, v, alias) & rw) | (u->sm_watchdog & ~rw & ~v);
        }
        break;
    default: break;
    }
    update(m);
}

// ------------------------------------------------------------------ The bus

typedef struct {
    bool enabled, dbl, int1, int2, int_nak, int_stall;
    unsigned type, bit, bc_off;
    uint32_t base;
} ep_t;

// Endpoint 0 has no control word of its own: SIE_CTRL configures it and its buffers are fixed.
static ep_t endpoint(chip_t *m, unsigned ep, bool out)
{
    ep_t e = {.bit = ep * 2 + out, .bc_off = 0x80 + ep * 8 + out * 4};
    uint32_t c;
    if (ep == 0) {
        uint32_t s = m->usb.sie_ctrl;
        c = (1u << 31) | ((s & SIE_EP0_DOUBLE_BUF) ? 1u << 30 : 0) | ((s & SIE_EP0_INT_1BUF) ? 1u << 29 : 0) |
            ((s & SIE_EP0_INT_2BUF) ? 1u << 28 : 0) | ((s & SIE_EP0_INT_STALL) ? 1u << 17 : 0) |
            ((s & SIE_EP0_INT_NAK) ? 1u << 16 : 0) | 0x100;
    } else {
        c = rd32le(m->usb_dpram + ep * 8 + out * 4);
    }
    e.enabled = c >> 31;
    e.dbl = (c >> 30) & 1;
    e.int1 = (c >> 29) & 1;
    e.int2 = (c >> 28) & 1;
    e.type = (c >> 26) & 3;
    e.int_stall = (c >> 17) & 1;
    e.int_nak = (c >> 16) & 1;
    e.base = c & 0xfc0;
    return e;
}

static uint32_t buf_offset(chip_t *m, const ep_t *e, unsigned sel)
{
    if (!sel)
        return e->base;
    if (e->type == EP_TYPE_ISO)
        return (e->base + (128u << ((rd32le(m->usb_dpram + e->bc_off) >> 27) & 3))) & 0xfff;
    return (e->base + 64) & 0xfff;
}

// The buffer the next transaction uses, after any reset of the select that software asked for.
static unsigned select_buffer(chip_t *m, const ep_t *e, uint32_t bc)
{
    usb_t *u = &m->usb;
    if (bc & BC_RESET_SEL)
        u->buf_sel &= ~(1u << e->bit);
    return e->dbl ? (u->buf_sel >> e->bit) & 1 : 0;
}

static bool token_ok(chip_t *m, unsigned addr)
{
    usb_t *u = &m->usb;
    if (!u->host_present || !u->listening || u->host_reset)
        return false;
    activity(m);
    return addr == (u->addr_endp[0] & 0x7f);
}

// Whether the endpoint takes the transaction (USB_ACK) or what it answers instead. An aborted
// endpoint ignores its buffer control word, stall included.
static int refuse(chip_t *m, const ep_t *e, uint16_t half, unsigned ep, bool out)
{
    usb_t *u = &m->usb;
    bool aborted = u->ep_abort & (1u << e->bit);
    bool armed = ep != 0 || (u->ep_stall_arm & (out ? 2u : 1u));
    if (!aborted && (half & BC_STALL) && armed) {
        if (e->int_stall)
            u->stall_nak |= 1u << e->bit;
        return USB_STALL;
    }
    bool ready = (half & BC_AVAILABLE) && (out ? !(half & BC_FULL) : (half & BC_FULL));
    if (!aborted && ready)
        return USB_ACK;
    if (e->type == EP_TYPE_ISO)
        return USB_NONE;
    if (e->int_nak)
        u->stall_nak |= 1u << e->bit;
    return USB_NAK;
}

// The controller writes back its half of the buffer control word, then flags the buffer.
static void finish(chip_t *m, const ep_t *e, unsigned sel, uint16_t wb, bool flip)
{
    usb_t *u = &m->usb;
    uint8_t *p = m->usb_dpram + e->bc_off + 2 * sel;
    if (sel)
        wb |= rd16le(p) & (3u << 11);
    wr16le(p, wb);
    if (wb & BC_LAST)
        u->sie_status |= ST_TRANS_COMPLETE;
    if (e->dbl && flip)
        u->buf_sel ^= 1u << e->bit;
    bool flag = e->int1;
    if (!flag && e->int2 && e->dbl) {
        u->odd_done ^= 1u << e->bit;
        flag = !(u->odd_done & (1u << e->bit));
    }
    if (flag)
        flag_buffer(m, e->bit, sel);
}

void usb_bus_attach(chip_t *m, bool present)
{
    usb_t *u = &m->usb;
    u->host_present = present;
    u->host_reset = false;
    update(m);
    // A device already pulling D+ up is seen at once by a host plugged in after it.
    if (present && u->pullup)
        usbhost_pullup(m, true);
}

void usb_bus_reset(chip_t *m, bool active)
{
    m->usb.host_reset = active;
    update(m);
}

void usb_bus_sof(chip_t *m, unsigned frame)
{
    usb_t *u = &m->usb;
    if (!u->host_present || !u->listening || u->host_reset)
        return;
    activity(m);
    u->sof_rd = frame & 0x7ff;
    u->sof_irq = true;
    u->sof_ts_last = (uint32_t)(m->now / PS_PER_NS * 48 / 1000) & 0x1fffff;
    irq_update(m);
}

int usb_bus_setup(chip_t *m, unsigned addr, const uint8_t pkt[8])
{
    usb_t *u = &m->usb;
    if (!token_ok(m, addr))
        return USB_NONE;
    memcpy(m->usb_dpram, pkt, 8);
    // A SETUP clears EP0's stall, as USB requires.
    u->ep_stall_arm = 0;
    u->sie_status |= ST_SETUP_REC;
    irq_update(m);
    return USB_ACK;
}

int usb_bus_in(chip_t *m, unsigned addr, unsigned ep, uint8_t *buf, unsigned *len, unsigned *pid)
{
    *len = 0;
    if (ep > 15 || !token_ok(m, addr))
        return USB_NONE;
    ep_t e = endpoint(m, ep, false);
    if (!e.enabled)
        return USB_NONE;
    uint32_t bc = rd32le(m->usb_dpram + e.bc_off);
    unsigned sel = select_buffer(m, &e, bc);
    uint16_t half = (uint16_t)(bc >> (16 * sel));
    int r = refuse(m, &e, half, ep, false);
    if (r != USB_ACK) {
        irq_update(m);
        return r;
    }
    unsigned n = half & BC_LEN;
    uint32_t off = buf_offset(m, &e, sel);
    for (unsigned i = 0; i < n; i++)
        buf[i] = m->usb_dpram[(off + i) & 0xfff];
    *len = n;
    *pid = (half & BC_PID) ? 1 : 0;
    finish(m, &e, sel, (uint16_t)(n | (half & (BC_PID | BC_LAST))), true);
    irq_update(m);
    return USB_ACK;
}

int usb_bus_out(chip_t *m, unsigned addr, unsigned ep, unsigned pid, const uint8_t *data, unsigned len)
{
    usb_t *u = &m->usb;
    if (ep > 15 || !token_ok(m, addr))
        return USB_NONE;
    ep_t e = endpoint(m, ep, true);
    if (!e.enabled)
        return USB_NONE;
    uint32_t bc = rd32le(m->usb_dpram + e.bc_off);
    unsigned sel = select_buffer(m, &e, bc);
    uint16_t half = (uint16_t)(bc >> (16 * sel));
    int r = refuse(m, &e, half, ep, true);
    if (r != USB_ACK) {
        irq_update(m);
        return r;
    }
    // A packet with the wrong toggle is a repeat of one whose ACK the host lost: acknowledged again
    // and dropped.
    if (e.type != EP_TYPE_ISO && pid != ((half & BC_PID) ? 1u : 0u)) {
        u->sie_status |= ST_DATA_SEQ_ERROR;
        irq_update(m);
        return USB_ACK;
    }
    unsigned cap = half & BC_LEN;
    if (len > cap) {
        u->sie_status |= ST_RX_OVERFLOW;
        irq_update(m);
        return USB_NONE;
    }
    uint32_t off = buf_offset(m, &e, sel);
    for (unsigned i = 0; i < len; i++)
        m->usb_dpram[(off + i) & 0xfff] = data[i];
    bool flip = true;
    if (len < cap && !m->rp2040) {
        u->sie_status |= ST_RX_SHORT_PACKET;
        flip = !(ep == 0 && (u->sie_ctrl & SIE_EP0_STOP_ON_SHORT));
    }
    finish(m, &e, sel, (uint16_t)(len | (pid ? BC_PID : 0) | (half & BC_LAST) | BC_FULL), flip);
    irq_update(m);
    return USB_ACK;
}
