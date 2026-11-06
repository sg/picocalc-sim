#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sock.h"
#include "usbhost.h"
#ifndef _WIN32
#include <netinet/tcp.h>
#endif

// A USB/IP server, as usbipd is, exporting the board's USB device to a computer's own USB stack:
// `usbip attach -r HOST -b 1-1` there, and its drivers see the device as if it were plugged in.
// The host on the cable stays the device's host: it enumerated the device, keeps the bus awake and
// runs each URB the computer sends as transactions on the emulated bus, one queue per endpoint.
// What the computer asks of the device's state, its configuration, interfaces, halts and resets,
// the server does itself as Linux's stub driver does, keeping the data toggles in step. Its own
// serial and drive drivers stand aside while the device is exported. Sockets are polled from an
// event, as the WiFi's are, so everything runs on the emulation thread.

enum { C_NONE, C_OP, C_RESETTING, C_URB };
enum { ST_ESTALL = -32, ST_EPROTO = -71, ST_EOVERFLOW = -75, ST_ECONNRESET = -104, ST_EREMOTEIO = -121,
       ST_EINVAL = -22 };
enum { URB_SHORT_NOT_OK = 0x1, URB_ZERO_PACKET = 0x40 };
enum { CT_SETUP, CT_DATA, CT_STATUS, CT_ZLP };
// The reset the server does on import, and on a port reset request.
enum { RS_ASSERT, RS_RELEASE, RS_ADDRESS, RS_CONFIG, RS_DONE };

#define IDLE_POLL_PS (10 * PS_PER_MS)
#define BUSY_POLL_PS (250 * PS_PER_US)
#define MAX_URB (4u << 20)

typedef struct urb {
    struct urb *next;
    uint32_t seqnum, flags, len, interval;
    unsigned ep, moved, stage, toggle;
    bool in;
    uint8_t setup[8];
    uint8_t *data;
} urb_t;

typedef struct usbip_pipe {
    struct usbip *ip;
    urb_t *head, *tail;
    emu_event_t ev;
    unsigned naks, errors;
} pipe_t;

typedef struct usbip {
    usb_host_t *h;
    sock_t listen_fd, fd;
    int conn;
    uint16_t version;
    emu_event_t poll_ev, reset_ev;
    uint8_t *in;
    size_t in_len, in_cap;
    uint8_t *out;
    size_t out_len, out_cap;
    // Endpoint n's IN and OUT pipes at 2n+1 and 2n; endpoint 0's control pipe at 0.
    pipe_t pipes[32];
    uint8_t toggle[32], type[32];
    unsigned mps[32];
    // A reset under way, the address the device gets back after it, and the port reset request it
    // answers, if any.
    int reset_step;
    unsigned reset_addr;
    bool resetting;
    urb_t *reset_urb;
    char where[64];
} usbip_t;

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static int pipe_index(unsigned ep, bool in)
{
    return ep ? (int)(ep * 2 + (in ? 1 : 0)) : 0;
}

static void flush(usbip_t *ip)
{
    while (ip->out_len && ip->fd != SOCK_NONE) {
        ssize_t n = sock_send(ip->fd, ip->out, ip->out_len);
        if (n <= 0)
            return;
        memmove(ip->out, ip->out + n, ip->out_len - (size_t)n);
        ip->out_len -= (size_t)n;
    }
}

static void emit(usbip_t *ip, const void *data, size_t len)
{
    if (ip->out_len + len > ip->out_cap) {
        ip->out_cap = (ip->out_len + len) * 2;
        ip->out = realloc(ip->out, ip->out_cap);
    }
    memcpy(ip->out + ip->out_len, data, len);
    ip->out_len += len;
    flush(ip);
}

static void free_urbs(usbip_t *ip)
{
    for (int i = 0; i < 32; i++) {
        pipe_t *p = &ip->pipes[i];
        ev_cancel(ip->h->m, &p->ev);
        while (p->head) {
            urb_t *u = p->head;
            p->head = u->next;
            free(u->data);
            free(u);
        }
        p->tail = NULL;
    }
    free(ip->reset_urb ? ip->reset_urb->data : NULL);
    free(ip->reset_urb);
    ip->reset_urb = NULL;
}

static void drop_client(usbip_t *ip, const char *why)
{
    if (ip->fd == SOCK_NONE)
        return;
    bool exported = ip->conn == C_URB || ip->conn == C_RESETTING;
    flush(ip);
    sock_close(ip->fd);
    ip->fd = SOCK_NONE;
    ip->conn = C_NONE;
    ip->resetting = false;
    ip->in_len = ip->out_len = 0;
    free_urbs(ip);
    ev_cancel(ip->h->m, &ip->reset_ev);
    if (exported) {
        usbhost_log(ip->h, "usbip: %s; the device is no longer exported", why);
        usbhost_unexported(ip->h);
    }
}

// The endpoints of the configuration, for their types and packet sizes.
static void read_endpoints(usbip_t *ip)
{
    usb_host_t *h = ip->h;
    memset(ip->type, 0, sizeof(ip->type));
    ip->mps[0] = h->mps0;
    for (unsigned i = 0; i + 2 <= h->cfg_len && h->cfg[i] >= 2; i += h->cfg[i]) {
        const uint8_t *e = h->cfg + i;
        if (e[1] == 5 && e[0] >= 7) {
            int k = pipe_index(e[2] & 15, e[2] & 0x80);
            ip->type[k] = e[3] & 3;
            ip->mps[k] = (e[4] | e[5] << 8) & 0x7ff;
        }
    }
}

static void reset_toggles(usbip_t *ip)
{
    memset(ip->toggle, 0, sizeof(ip->toggle));
}

// ------------------------------------------------------------------ Device descriptions

static void device_block(usbip_t *ip, uint8_t *b, bool interfaces, size_t *len)
{
    usb_host_t *h = ip->h;
    memset(b, 0, 0x138);
    snprintf((char *)b, 256, "/sys/devices/platform/picocalc-sim/usb1/1-1");
    snprintf((char *)b + 0x100, 32, "1-1");
    put32(b + 0x120, 1);
    put32(b + 0x124, h->addr);
    put32(b + 0x128, 2);
    put16(b + 0x12c, h->dev[8] | h->dev[9] << 8);
    put16(b + 0x12e, h->dev[10] | h->dev[11] << 8);
    put16(b + 0x130, h->dev[12] | h->dev[13] << 8);
    b[0x132] = h->dev[4];
    b[0x133] = h->dev[5];
    b[0x134] = h->dev[6];
    b[0x135] = h->cfg[5];
    b[0x136] = h->dev[17];
    b[0x137] = h->cfg[4];
    *len = 0x138;
    if (!interfaces)
        return;
    for (unsigned i = 0; i + 2 <= h->cfg_len && h->cfg[i] >= 2; i += h->cfg[i]) {
        const uint8_t *e = h->cfg + i;
        if (e[1] == 4 && e[0] >= 9 && e[3] == 0) {
            b[*len] = e[5];
            b[*len + 1] = e[6];
            b[*len + 2] = e[7];
            b[*len + 3] = 0;
            *len += 4;
        }
    }
}

static bool exportable(usbip_t *ip)
{
    return ip->h->configured;
}

static void reply_devlist(usbip_t *ip)
{
    uint8_t b[12 + 0x138 + 4 * 32];
    put16(b, ip->version);
    put16(b + 2, 0x0005);
    put32(b + 4, 0);
    size_t len = 0;
    bool any = exportable(ip);
    put32(b + 8, any ? 1 : 0);
    if (any)
        device_block(ip, b + 12, true, &len);
    emit(ip, b, 12 + len);
}

static void reply_import(usbip_t *ip, bool ok)
{
    uint8_t b[8 + 0x138];
    put16(b, ip->version);
    put16(b + 2, 0x0003);
    put32(b + 4, ok ? 0 : 1);
    size_t len = 0;
    if (ok)
        device_block(ip, b + 8, false, &len);
    emit(ip, b, 8 + len);
}

// ------------------------------------------------------------------ URBs

static void ret_submit(usbip_t *ip, urb_t *u, int status, uint32_t actual)
{
    uint8_t b[48];
    memset(b, 0, sizeof(b));
    put32(b, 3);
    put32(b + 4, u->seqnum);
    put32(b + 20, (uint32_t)status);
    put32(b + 24, actual);
    put32(b + 32, 0xffffffffu);
    emit(ip, b, sizeof(b));
    if (u->in && actual)
        emit(ip, u->data, actual);
}

static void schedule_pipe(pipe_t *p, uint64_t after)
{
    if (p->head)
        ev_schedule(p->ip->h->m, &p->ev, p->ip->h->m->now + after);
}

static void complete(pipe_t *p, int status)
{
    urb_t *u = p->head;
    p->head = u->next;
    if (!p->head)
        p->tail = NULL;
    ret_submit(p->ip, u, status, u->moved);
    free(u->data);
    free(u);
    p->naks = p->errors = 0;
    schedule_pipe(p, usbhost_bus_time(0));
}

static void start_reset(usbip_t *ip, urb_t *answering);

// What the stub driver does itself once a control request has gone through.
static bool after_control(usbip_t *ip, urb_t *u)
{
    const uint8_t *s = u->setup;
    if (s[0] == 0x00 && s[1] == 9) {
        ip->h->cfg[5] = s[2];
        reset_toggles(ip);
    } else if (s[0] == 0x01 && s[1] == 11) {
        reset_toggles(ip);
    } else if (s[0] == 0x02 && s[1] == 1 && s[2] == 0 && s[3] == 0) {
        ip->toggle[pipe_index(s[4] & 15, s[4] & 0x80)] = 0;
    }
    return true;
}

static int answer(int r)
{
    return r == USB_STALL ? ST_ESTALL : ST_EPROTO;
}

// One transaction of the control URB at the head of endpoint 0's pipe.
static int control_xact(usbip_t *ip, urb_t *u, uint64_t *t)
{
    chip_t *m = ip->h->m;
    unsigned addr = ip->h->addr, mps = ip->mps[0] ? ip->mps[0] : 64;
    uint8_t buf[1023];
    unsigned n = 0, pid = 0;
    int r;
    switch (u->stage) {
    case CT_SETUP:
        r = usb_bus_setup(m, addr, u->setup);
        *t = usbhost_bus_time(8);
        if (r == USB_ACK) {
            u->toggle = 1;
            u->stage = u->len ? CT_DATA : CT_STATUS;
        }
        return r;
    case CT_DATA:
        if (u->in) {
            r = usb_bus_in(m, addr, 0, buf, &n, &pid);
            *t = usbhost_bus_time(n);
            if (r == USB_ACK && pid == u->toggle) {
                u->toggle ^= 1;
                unsigned take = n < u->len - u->moved ? n : u->len - u->moved;
                memcpy(u->data + u->moved, buf, take);
                u->moved += take;
                if (n < mps || u->moved >= u->len)
                    u->stage = CT_STATUS;
            }
        } else {
            n = u->len - u->moved < mps ? u->len - u->moved : mps;
            r = usb_bus_out(m, addr, 0, u->toggle, u->data + u->moved, n);
            *t = usbhost_bus_time(n);
            if (r == USB_ACK) {
                u->toggle ^= 1;
                u->moved += n;
                if (u->moved >= u->len)
                    u->stage = CT_STATUS;
            }
        }
        return r;
    default:
        if (u->in && u->len)
            r = usb_bus_out(m, addr, 0, 1, NULL, 0);
        else
            r = usb_bus_in(m, addr, 0, buf, &n, &pid);
        *t = usbhost_bus_time(0);
        if (r == USB_ACK)
            u->stage = CT_ZLP;
        return r;
    }
}

static void pipe_run(chip_t *m, emu_event_t *ev)
{
    pipe_t *p = ev->ctx;
    usbip_t *ip = p->ip;
    urb_t *u = p->head;
    // A reset under way holds every pipe, and starts them again when it is done.
    if (!u || ip->conn != C_URB || ip->resetting)
        return;
    int k = (int)(p - ip->pipes);
    uint64_t t = usbhost_bus_time(0);
    uint8_t buf[1023];
    unsigned n = 0, pid = 0, mps = ip->mps[k] ? ip->mps[k] : 64;
    int r;
    if (k == 0) {
        r = control_xact(ip, u, &t);
        if (u->stage == CT_ZLP) {
            after_control(ip, u);
            complete(p, 0);
            return;
        }
    } else if (u->in) {
        r = usb_bus_in(m, ip->h->addr, u->ep, buf, &n, &pid);
        t = usbhost_bus_time(n);
        if (r == USB_ACK && pid == ip->toggle[k]) {
            ip->toggle[k] ^= 1;
            if (n > u->len - u->moved) {
                complete(p, ST_EOVERFLOW);
                return;
            }
            memcpy(u->data + u->moved, buf, n);
            u->moved += n;
            if (n < mps || u->moved >= u->len) {
                bool short_bad = (u->flags & URB_SHORT_NOT_OK) && u->moved < u->len;
                complete(p, short_bad ? ST_EREMOTEIO : 0);
                return;
            }
        }
    } else {
        n = u->len - u->moved < mps ? u->len - u->moved : mps;
        r = usb_bus_out(m, ip->h->addr, u->ep, ip->toggle[k], u->data + u->moved, n);
        t = usbhost_bus_time(n);
        if (r == USB_ACK) {
            ip->toggle[k] ^= 1;
            u->moved += n;
            // A transfer that fills its last packet ends with a zero-length one if asked to.
            bool zlp = (u->flags & URB_ZERO_PACKET) && n == mps && u->moved >= u->len;
            if (u->moved >= u->len && !zlp) {
                complete(p, 0);
                return;
            }
            if (zlp)
                u->flags &= ~(uint32_t)URB_ZERO_PACKET;
        }
    }
    switch (r) {
    case USB_ACK:
        p->naks = p->errors = 0;
        break;
    case USB_NAK:
        t = usbhost_nak_delay(++p->naks);
        // An interrupt endpoint is polled once a period.
        if (ip->type[k] == 3 && u->interval && t < u->interval * PS_PER_MS)
            t = u->interval * PS_PER_MS;
        break;
    case USB_STALL: complete(p, ST_ESTALL); return;
    default:
        if (++p->errors >= 3) {
            complete(p, answer(r));
            return;
        }
        break;
    }
    ev_schedule(m, ev, m->now + t);
}

static void submit(usbip_t *ip, const uint8_t *hdr, const uint8_t *data)
{
    urb_t *u = calloc(1, sizeof(*u));
    u->seqnum = be32(hdr + 4);
    u->in = be32(hdr + 12) == 1;
    u->ep = be32(hdr + 16) & 15;
    u->flags = be32(hdr + 20);
    u->len = be32(hdr + 24);
    u->interval = be32(hdr + 36);
    memcpy(u->setup, hdr + 40, 8);
    u->data = malloc(u->len ? u->len : 1);
    if (!u->in && u->len)
        memcpy(u->data, data, u->len);
    int k = pipe_index(u->ep, u->in);
    if (u->ep == 0)
        u->in = u->setup[0] & 0x80;
    if (ip->h->debug)
        usbhost_log(ip->h, "usbip: submit %u: ep %u %s, %u bytes%s", u->seqnum, u->ep, u->in ? "in" : "out", u->len,
                    u->ep ? "" : " (control)");
    if (u->ep && ip->type[k] == 1) {
        // Isochronous transfers are not carried.
        ret_submit(ip, u, ST_EINVAL, 0);
        free(u->data);
        free(u);
        return;
    }
    // A request to reset the port, which the stub driver answers by resetting the device.
    if (u->ep == 0 && u->setup[0] == 0x23 && u->setup[1] == 3 && u->setup[2] == 4) {
        start_reset(ip, u);
        return;
    }
    pipe_t *p = &ip->pipes[k];
    if (p->tail)
        p->tail->next = u;
    else
        p->head = u;
    p->tail = u;
    if (p->head == u)
        schedule_pipe(p, usbhost_bus_time(0));
}

static void unlink_urb(usbip_t *ip, const uint8_t *hdr)
{
    uint32_t seq = be32(hdr + 4), target = be32(hdr + 20);
    int status = 0;
    for (int i = 0; i < 32 && !status; i++) {
        pipe_t *p = &ip->pipes[i];
        urb_t *prev = NULL;
        for (urb_t *u = p->head; u; prev = u, u = u->next) {
            if (u->seqnum != target)
                continue;
            if (prev)
                prev->next = u->next;
            else
                p->head = u->next;
            if (p->tail == u)
                p->tail = prev;
            free(u->data);
            free(u);
            status = ST_ECONNRESET;
            p->naks = p->errors = 0;
            if (!prev)
                schedule_pipe(p, usbhost_bus_time(0));
            break;
        }
    }
    uint8_t b[48];
    memset(b, 0, sizeof(b));
    put32(b, 4);
    put32(b + 4, seq);
    put32(b + 20, (uint32_t)status);
    emit(ip, b, sizeof(b));
}

// ------------------------------------------------------------------ Resetting the device

static void reset_ctl_done(usb_host_t *h, int r);
static void parse(usbip_t *ip);

static void reset_step(chip_t *m, emu_event_t *ev)
{
    usbip_t *ip = ev->ctx;
    usb_host_t *h = ip->h;
    switch (ip->reset_step) {
    case RS_ASSERT:
        usb_bus_reset(m, true);
        ip->reset_step = RS_RELEASE;
        ev_schedule(m, ev, m->now + 20 * PS_PER_MS);
        return;
    case RS_RELEASE:
        usb_bus_reset(m, false);
        ip->reset_step = RS_ADDRESS;
        ev_schedule(m, ev, m->now + 10 * PS_PER_MS);
        return;
    case RS_ADDRESS:
        // A reset device answers at address 0 until it is given its own back.
        h->addr = 0;
        if (!usbhost_control(h, 0x00, 5, (uint16_t)ip->reset_addr, 0, 0, reset_ctl_done)) {
            h->addr = ip->reset_addr;
            ev_schedule(m, ev, m->now + PS_PER_MS);
        }
        return;
    case RS_CONFIG:
        if (!usbhost_control(h, 0x00, 9, h->cfg[5], 0, 0, reset_ctl_done))
            ev_schedule(m, ev, m->now + PS_PER_MS);
        return;
    default: break;
    }
    reset_toggles(ip);
    read_endpoints(ip);
    ip->resetting = false;
    if (ip->conn == C_RESETTING) {
        ip->conn = C_URB;
        reply_import(ip, true);
        usbhost_log(h, "usbip: the device is exported to %s", ip->where);
        parse(ip);
    }
    if (ip->reset_urb) {
        urb_t *u = ip->reset_urb;
        ip->reset_urb = NULL;
        ret_submit(ip, u, 0, 0);
        free(u->data);
        free(u);
        for (int i = 0; i < 32; i++)
            schedule_pipe(&ip->pipes[i], usbhost_bus_time(0));
    }
}

static void reset_ctl_done(usb_host_t *h, int r)
{
    usbip_t *ip = h->usbip;
    if (ip->reset_step == RS_ADDRESS)
        h->addr = ip->reset_addr;
    if (r != XF_OK && ip->reset_step == RS_ADDRESS) {
        drop_client(ip, "the device did not take its address after a reset");
        return;
    }
    // The device has 10 ms to take its new address, as after enumeration.
    ip->reset_step = ip->reset_step == RS_ADDRESS ? RS_CONFIG : RS_DONE;
    ev_schedule(h->m, &ip->reset_ev, h->m->now + (ip->reset_step == RS_CONFIG ? 10 * PS_PER_MS : 0));
}

static void start_reset(usbip_t *ip, urb_t *answering)
{
    free(ip->reset_urb ? ip->reset_urb->data : NULL);
    free(ip->reset_urb);
    ip->reset_urb = answering;
    for (int i = 0; i < 32; i++)
        ev_cancel(ip->h->m, &ip->pipes[i].ev);
    ip->reset_addr = ip->h->addr ? ip->h->addr : 1;
    ip->resetting = true;
    ip->reset_step = RS_ASSERT;
    ev_schedule(ip->h->m, &ip->reset_ev, ip->h->m->now);
}

// ------------------------------------------------------------------ The socket

static bool need(usbip_t *ip, size_t n)
{
    return ip->in_len >= n;
}

// Takes whole messages off the front of what the client has sent.
static void parse(usbip_t *ip)
{
    for (;;) {
        size_t used = 0;
        if (ip->conn == C_OP) {
            if (!need(ip, 8))
                return;
            ip->version = (uint16_t)(ip->in[0] << 8 | ip->in[1]);
            unsigned code = ip->in[2] << 8 | ip->in[3];
            if (code == 0x8005) {
                reply_devlist(ip);
                ip->in_len = 0;
                drop_client(ip, "listed");
                return;
            }
            if (code != 0x8003) {
                drop_client(ip, "an unknown request");
                return;
            }
            if (!need(ip, 40))
                return;
            char busid[33];
            memcpy(busid, ip->in + 8, 32);
            busid[32] = 0;
            used = 40;
            if (strcmp(busid, "1-1") != 0 || !exportable(ip)) {
                reply_import(ip, false);
                ip->in_len = 0;
                drop_client(ip, "refused");
                return;
            }
            // The device starts afresh for its new host, from a reset and on DATA0 everywhere.
            usbhost_exported(ip->h);
            ip->conn = C_RESETTING;
            start_reset(ip, NULL);
        } else if (ip->conn == C_URB) {
            if (!need(ip, 48))
                return;
            uint32_t cmd = be32(ip->in);
            if (cmd == 1) {
                bool out = be32(ip->in + 12) == 0;
                uint32_t len = be32(ip->in + 24), packets = be32(ip->in + 32);
                if (len > MAX_URB) {
                    drop_client(ip, "an URB too big to take");
                    return;
                }
                size_t iso = packets && packets != 0xffffffffu ? (size_t)packets * 16 : 0;
                size_t total = 48 + (out ? len : 0) + iso;
                if (!need(ip, total))
                    return;
                submit(ip, ip->in, ip->in + 48);
                used = total;
            } else if (cmd == 2) {
                unlink_urb(ip, ip->in);
                used = 48;
            } else {
                drop_client(ip, "an unknown command");
                return;
            }
        } else {
            return;
        }
        memmove(ip->in, ip->in + used, ip->in_len - used);
        ip->in_len -= used;
    }
}

static void poll_socket(chip_t *m, emu_event_t *ev)
{
    usbip_t *ip = ev->ctx;
    if (ip->fd == SOCK_NONE && ip->listen_fd != SOCK_NONE) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        sock_t fd = accept(ip->listen_fd, (struct sockaddr *)&sa, &sl);
        if (fd != SOCK_NONE) {
            sock_set_nonblock(fd);
            sock_setopt(fd, IPPROTO_TCP, TCP_NODELAY, 1);
            ip->fd = fd;
            ip->conn = C_OP;
            ip->in_len = ip->out_len = 0;
            snprintf(ip->where, sizeof(ip->where), "%s:%u", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
        }
    } else if (ip->fd != SOCK_NONE && ip->listen_fd != SOCK_NONE) {
        // One client at a time, as one computer can have the device.
        sock_t fd = accept(ip->listen_fd, NULL, NULL);
        if (fd != SOCK_NONE)
            sock_close(fd);
    }
    while (ip->fd != SOCK_NONE) {
        if (ip->in_cap - ip->in_len < 65536) {
            ip->in_cap = ip->in_cap ? ip->in_cap * 2 : 1 << 17;
            ip->in = realloc(ip->in, ip->in_cap);
        }
        ssize_t n = sock_recv(ip->fd, ip->in + ip->in_len, ip->in_cap - ip->in_len);
        if (n > 0) {
            ip->in_len += (size_t)n;
            parse(ip);
            continue;
        }
        if (n == 0 || !sock_again(sock_errno()))
            drop_client(ip, "the computer let go of it");
        break;
    }
    flush(ip);
    ev_schedule(m, ev, m->now + (ip->fd != SOCK_NONE ? BUSY_POLL_PS : IDLE_POLL_PS));
}

// ------------------------------------------------------------------ The host's side

bool usbip_open(usb_host_t *h, const char *spec)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", spec);
    uint32_t bind_ip = INADDR_LOOPBACK;
    char *port_s = buf, *colon = strrchr(buf, ':');
    if (colon) {
        *colon = 0;
        port_s = colon + 1;
        struct in_addr a;
        if (!strcmp(buf, "*"))
            bind_ip = INADDR_ANY;
        else if (inet_pton(AF_INET, buf, &a) == 1)
            bind_ip = ntohl(a.s_addr);
        else {
            picocalc_sim_log("usb: --usbip %s: %s is not an IPv4 address", spec, buf);
            return false;
        }
    }
    char *end;
    long port = strtol(port_s, &end, 10);
    if (*end || port <= 0 || port > 65535) {
        picocalc_sim_log("usb: --usbip %s: no port to listen on", spec);
        return false;
    }
    sock_startup();
    sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == SOCK_NONE)
        return false;
#ifndef _WIN32
    sock_setopt(fd, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(bind_ip)};
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, 4) != 0 || !sock_set_nonblock(fd)) {
        picocalc_sim_log("usb: USB/IP cannot listen on port %ld: %s", port, sock_strerror(sock_errno()));
        sock_close(fd);
        return false;
    }
    usbip_t *ip = calloc(1, sizeof(*ip));
    ip->h = h;
    ip->listen_fd = fd;
    ip->fd = SOCK_NONE;
    ip->poll_ev = (emu_event_t){.fn = poll_socket, .ctx = ip};
    ip->reset_ev = (emu_event_t){.fn = reset_step, .ctx = ip};
    for (int i = 0; i < 32; i++)
        ip->pipes[i] = (pipe_t){.ip = ip, .ev = {.fn = pipe_run, .ctx = &ip->pipes[i]}};
    h->usbip = ip;
    ev_schedule(h->m, &ip->poll_ev, h->m->now + IDLE_POLL_PS);
    picocalc_sim_log("usb: USB/IP server on %s:%ld; attach with: usbip attach -r HOST -b 1-1", bind_ip == INADDR_ANY ? "*" : inet_ntoa(sa.sin_addr), port);
    return true;
}

void usbip_close(usb_host_t *h)
{
    usbip_t *ip = h->usbip;
    if (!ip)
        return;
    drop_client(ip, "picocalc-sim is closing");
    ev_cancel(h->m, &ip->poll_ev);
    sock_close(ip->listen_fd);
    free(ip->in);
    free(ip->out);
    free(ip);
    h->usbip = NULL;
}

// Events go when the chip resets; the server keeps polling.
void usbip_resume(usb_host_t *h)
{
    usbip_t *ip = h->usbip;
    if (ip && !ip->poll_ev.queued)
        ev_schedule(h->m, &ip->poll_ev, h->m->now + IDLE_POLL_PS);
}

bool usbip_exported(usb_host_t *h)
{
    usbip_t *ip = h->usbip;
    return ip && (ip->conn == C_URB || ip->conn == C_RESETTING);
}

// The device has disconnected, which the computer sees as it being unplugged.
void usbip_device_gone(usb_host_t *h)
{
    usbip_t *ip = h->usbip;
    if (ip && usbip_exported(h))
        drop_client(ip, "the device disconnected");
}
