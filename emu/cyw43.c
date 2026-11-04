#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chip.h"
#include "pcsim/bus.h"
#include "pcsim/net.h"
#include "pcsim/sim.h"

// The W boards' CYW43439 as the SDK's cyw43 driver sees it over gSPI: the bus registers, the
// backplane the radio firmware is downloaded into, and the SDPCM control, event and data channels
// that firmware serves once it runs. The firmware itself is not run. Ioctls are answered here,
// scans and joins see a list of virtual access points, and Ethernet frames go to the user-mode
// network in sim/net.

#define PIN_REG_ON 23
#define PIN_DATA 24
#define PIN_CS 25
#define PIN_CLK 29

#define RAM_SIZE (512 * 1024)
#define BUF_MAX 2048
#define PKT_MAX 1536
#define QUEUE_MAX 96
#define CREDIT_WINDOW 8
#define POLL_PS (1 * PS_PER_MS)

#define BUS_WORD32 0x01
#define BUS_IRQ_HIGH 0x20
#define INT_F2_PACKET 0x0020
#define STATUS_F2_RX_READY 0x20
#define STATUS_F2_PKT 0x100

#define F1_WATERMARK 0x10008
#define F1_ADDR_LOW 0x1000a
#define F1_ADDR_MID 0x1000b
#define F1_ADDR_HIGH 0x1000c
#define F1_FRAME_CTRL 0x1000d
#define F1_CLOCK_CSR 0x1000e
#define F1_PULL_UP 0x1000f
#define F1_WAKEUP 0x1001e
#define F1_SLEEP_CSR 0x1001f
#define CSR_ALP_AVAIL 0x40
#define CSR_HT_AVAIL 0x80

#define ARM_WRAPPER 0x18103000u
#define SRAM_WRAPPER 0x18104000u
#define AI_IOCTRL 0x408
#define AI_RESETCTRL 0x800
#define SICF_CPUHALT 0x20
#define SICF_FGC 0x02
#define SICF_CLOCK_EN 0x01

enum { CH_CONTROL, CH_EVENT, CH_DATA };

enum {
    WLC_UP = 2,
    WLC_DOWN = 3,
    WLC_GET_BSSID = 23,
    WLC_GET_SSID = 25,
    WLC_SET_SSID = 26,
    WLC_GET_CHANNEL = 29,
    WLC_DISASSOC = 52,
    WLC_GET_PM = 85,
    WLC_SET_PM = 86,
    WLC_GET_RSSI = 127,
    WLC_SET_WSEC = 134,
    WLC_GET_ASSOCLIST = 159,
    WLC_SET_WPA_AUTH = 165,
    WLC_GET_VAR = 262,
    WLC_SET_VAR = 263,
    WLC_SET_WSEC_PMK = 268,
};

enum {
    EV_SET_SSID = 0,
    EV_JOIN = 1,
    EV_AUTH = 3,
    EV_ASSOC = 7,
    EV_DISASSOC = 11,
    EV_LINK = 16,
    EV_PSK_SUP = 46,
    EV_ESCAN_RESULT = 69,
};

enum { ST_SUCCESS = 0, ST_FAIL = 1, ST_NO_NETWORKS = 3, ST_PARTIAL = 8 };
#define WLC_SUP_KEYED 6
#define WLC_SUP_KEYXCHANGE_FAIL 4
#define WLC_E_SUP_DEAUTH 14

typedef struct pkt {
    struct pkt *next;
    size_t len;
    bool droppable;
    uint8_t buf[];
} pkt_t;

typedef struct {
    char ssid[33];
    uint8_t ssid_len;
    char pass[129];
    bool secured;
    bool any_pass;
    uint8_t bssid[6];
    uint8_t channel;
    int16_t rssi;
} ap_t;

typedef struct {
    char name[40];
    uint8_t val[64];
    size_t len;
} var_t;

typedef struct cyw43 {
    chip_t *m;
    bool powered, selected;

    bool word32;
    uint8_t f0[0x20];
    uint8_t in[BUF_MAX + 8];
    size_t in_len;
    int in_bits;
    uint8_t in_byte;
    uint32_t cmd;
    uint8_t out[BUF_MAX + 64];
    size_t out_len, out_pos;
    int out_bit;
    uint8_t out_cur;

    uint32_t window;
    uint8_t clock_csr, pull_up, sleep_csr, wakeup, watermark;
    uint8_t *ram;
    uint32_t sdio_int, sdio_host_mask, sdio_func_mask, bankx_index, bankx_pda;
    uint8_t arm_ioctrl, arm_reset, sram_ioctrl, sram_reset;
    bool booted;
    emu_event_t boot_ev;

    uint8_t host_seq, tx_seq, credit;
    pkt_t *qh, *qt;
    int qn;

    uint8_t mac[6];
    bool up;
    uint32_t wsec, wpa_auth, pm;
    char key[129];
    uint8_t evmask[19];
    var_t vars[48];
    int nvars;
    uint8_t mcast[4 + 60];
    bool led;

    ap_t aps[24];
    int naps;
    bool permissive;

    emu_event_t scan_ev;
    int scan_stage;
    uint8_t scan_ssid[32];
    uint32_t scan_ssid_len;
    uint16_t scan_sync;

    emu_event_t join_ev;
    uint8_t join_ssid[32];
    uint32_t join_ssid_len;
    uint8_t missing[32];
    uint32_t missing_len;
    int joined;
    bool link;

    emu_event_t poll_ev;
} cyw43_t;

static void update_irq(cyw43_t *w);
static bool debug;

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wrbe16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void wrbe32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// Until the host switches the bus to 32-bit words, each word arrives as two swapped halfwords.
static void swap16(uint8_t *p, size_t len)
{
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint8_t t = p[i];
        p[i] = p[i + 1];
        p[i + 1] = t;
    }
}

// ---------------------------------------------------------------------------------------------
// Virtual access points

static void add_ap(cyw43_t *w, const char *ssid, size_t ssid_len, const char *pass, bool any_pass)
{
    if (w->naps >= (int)(sizeof(w->aps) / sizeof(w->aps[0])) || ssid_len == 0 || ssid_len > 32)
        return;
    ap_t *a = &w->aps[w->naps];
    memset(a, 0, sizeof(*a));
    memcpy(a->ssid, ssid, ssid_len);
    a->ssid_len = (uint8_t)ssid_len;
    if (pass)
        snprintf(a->pass, sizeof(a->pass), "%s", pass);
    a->secured = pass != NULL;
    a->any_pass = any_pass;
    static const uint8_t base[6] = {0x02, 0x50, 0x43, 0x53, 0x49, 0x00};
    memcpy(a->bssid, base, 6);
    a->bssid[5] = (uint8_t)(0x10 + w->naps);
    a->channel = (uint8_t)(1 + 5 * (w->naps % 3));
    a->rssi = (int16_t)(-42 - 7 * w->naps);
    w->naps++;
}

static ap_t *find_ap(cyw43_t *w, const uint8_t *ssid, size_t len)
{
    for (int i = 0; i < w->naps; i++)
        if (w->aps[i].ssid_len == len && !memcmp(w->aps[i].ssid, ssid, len))
            return &w->aps[i];
    return NULL;
}

static void config_aps(cyw43_t *w)
{
    w->naps = 0;
    w->permissive = pcsim_opts.wifi_ap_count == 0;
    for (int i = 0; i < pcsim_opts.wifi_ap_count; i++) {
        const char *s = pcsim_opts.wifi_aps[i];
        const char *colon = strchr(s, ':');
        size_t n = colon ? (size_t)(colon - s) : strlen(s);
        if (n == 0 || n > 32) {
            pcsim_log("wifi: ignoring access point '%s' (the SSID must be 1-32 bytes)", s);
            continue;
        }
        add_ap(w, s, n, colon ? colon + 1 : NULL, false);
    }
    if (w->permissive)
        add_ap(w, "PicoCalc-Sim", 12, "", true);
}

// ---------------------------------------------------------------------------------------------
// Packets queued for the host on F2

// Events and data carry 6 bytes of header padding, which leaves the IP header of a received frame
// word-aligned in the driver's buffer; the driver's event parsing relies on that offset.
static size_t hdr_len(int chan)
{
    return chan == CH_CONTROL ? 12 : 18;
}

static pkt_t *pkt_new(cyw43_t *w, int chan, size_t body)
{
    size_t len = hdr_len(chan) + body;
    pkt_t *p = calloc(1, sizeof(*p) + len + 4);
    p->len = len;
    wr16(p->buf, (uint32_t)len);
    wr16(p->buf + 2, ~(uint32_t)len & 0xffff);
    p->buf[4] = w->tx_seq++;
    p->buf[5] = (uint8_t)chan;
    p->buf[7] = (uint8_t)hdr_len(chan);
    return p;
}

static void pkt_push(cyw43_t *w, pkt_t *p)
{
    if (p->droppable && w->qn >= QUEUE_MAX) {
        free(p);
        return;
    }
    if (w->qt)
        w->qt->next = p;
    else
        w->qh = p;
    w->qt = p;
    w->qn++;
    update_irq(w);
}

static void pkt_pop(cyw43_t *w)
{
    pkt_t *p = w->qh;
    if (!p)
        return;
    w->qh = p->next;
    if (!w->qh)
        w->qt = NULL;
    w->qn--;
    free(p);
}

static void queue_flush(cyw43_t *w)
{
    while (w->qh)
        pkt_pop(w);
}

// The host may send while its sequence number trails the credit it last heard; a header-only
// packet tops that up when a stream of data frames would otherwise stall it.
static void maybe_credit(cyw43_t *w)
{
    if ((uint8_t)(w->credit - w->host_seq) <= CREDIT_WINDOW / 2 && !w->qh)
        pkt_push(w, pkt_new(w, CH_CONTROL, 0));
}

static void send_event(cyw43_t *w, uint32_t type, uint32_t status, uint32_t reason, uint16_t flags,
                       const uint8_t *addr, const uint8_t *data, size_t dlen)
{
    if (type < 152 && !(w->evmask[type / 8] & (1u << (type % 8))))
        return;
    if (debug)
        pcsim_log("wifi: event %u status %u reason %u", type, status, reason);
    size_t body = 4 + 14 + 10 + 48 + dlen;
    if (hdr_len(CH_EVENT) + body > PKT_MAX)
        return;
    pkt_t *p = pkt_new(w, CH_EVENT, body);
    uint8_t *b = p->buf + hdr_len(CH_EVENT);
    b[0] = 0x20;
    uint8_t *e = b + 4;
    memcpy(e, w->mac, 6);
    memcpy(e + 6, w->mac, 6);
    wrbe16(e + 12, 0x886c);
    uint8_t *h = e + 14;
    wrbe16(h, 0x8001);
    wrbe16(h + 2, (uint32_t)(body - 4 - 14 - 4));
    h[5] = 0x00;
    h[6] = 0x10;
    h[7] = 0x18;
    wrbe16(h + 8, 1);
    uint8_t *ev = h + 10;
    wrbe16(ev, 2);
    wrbe16(ev + 2, flags);
    wrbe32(ev + 4, type);
    wrbe32(ev + 8, status);
    wrbe32(ev + 12, reason);
    wrbe32(ev + 20, (uint32_t)dlen);
    if (addr)
        memcpy(ev + 24, addr, 6);
    memcpy(ev + 30, "wl0", 3);
    if (dlen)
        memcpy(ev + 48, data, dlen);
    pkt_push(w, p);
}

static void rx_frame(void *ctx, const uint8_t *frame, size_t len)
{
    cyw43_t *w = ctx;
    size_t h = hdr_len(CH_DATA);
    if (!w->link || h + 4 + len > PKT_MAX)
        return;
    pkt_t *p = pkt_new(w, CH_DATA, 4 + len);
    p->droppable = true;
    p->buf[h] = 0x20;
    memcpy(p->buf + h + 4, frame, len);
    pkt_push(w, p);
}

// ---------------------------------------------------------------------------------------------
// Scanning and joining

static void scan_result(cyw43_t *w, const ap_t *a)
{
    uint8_t d[12 + 128 + 80];
    memset(d, 0, sizeof(d));
    uint8_t ie[80];
    size_t n = 0;
    ie[n++] = 0;
    ie[n++] = a->ssid_len;
    memcpy(ie + n, a->ssid, a->ssid_len);
    n += a->ssid_len;
    static const uint8_t rates[] = {1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24};
    memcpy(ie + n, rates, sizeof(rates));
    n += sizeof(rates);
    ie[n++] = 3;
    ie[n++] = 1;
    ie[n++] = a->channel;
    if (a->secured) {
        static const uint8_t rsn[] = {48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4,
                                      1, 0, 0x00, 0x0f, 0xac, 2, 0, 0};
        memcpy(ie + n, rsn, sizeof(rsn));
        n += sizeof(rsn);
    }
    uint32_t bss_len = 128 + (uint32_t)n;
    wr32(d, 12 + bss_len);
    wr32(d + 4, 109);
    wr16(d + 8, w->scan_sync);
    wr16(d + 10, 1);
    uint8_t *b = d + 12;
    wr32(b, 109);
    wr32(b + 4, bss_len);
    memcpy(b + 8, a->bssid, 6);
    wr16(b + 14, 100);
    wr16(b + 16, 0x0401 | (a->secured ? 0x0010 : 0));
    b[18] = a->ssid_len;
    memcpy(b + 19, a->ssid, a->ssid_len);
    wr32(b + 52, 8);
    memcpy(b + 56, rates + 2, 8);
    wr16(b + 72, 0x1000u | a->channel);
    b[76] = 1;
    wr16(b + 78, (uint16_t)a->rssi);
    b[80] = (uint8_t)-92;
    b[88] = a->channel;
    wr16(b + 116, 128);
    wr32(b + 120, (uint32_t)n);
    wr16(b + 124, (uint16_t)(a->rssi + 92));
    memcpy(b + 128, ie, n);
    send_event(w, EV_ESCAN_RESULT, ST_PARTIAL, 0, 0, a->bssid, d, 12 + bss_len);
}

static void scan_event(chip_t *m, emu_event_t *ev)
{
    cyw43_t *w = ev->ctx;
    if (!w->powered)
        return;
    if (w->scan_stage == 1) {
        for (int i = 0; i < w->naps; i++) {
            const ap_t *a = &w->aps[i];
            if (w->scan_ssid_len && (a->ssid_len != w->scan_ssid_len || memcmp(a->ssid, w->scan_ssid, a->ssid_len)))
                continue;
            scan_result(w, a);
        }
        w->scan_stage = 2;
        ev_schedule(m, &w->scan_ev, m->now + 900 * PS_PER_MS);
        return;
    }
    uint8_t d[12] = {0};
    wr32(d, 12);
    wr32(d + 4, 109);
    wr16(d + 8, w->scan_sync);
    send_event(w, EV_ESCAN_RESULT, ST_SUCCESS, 0, 0, NULL, d, sizeof(d));
    w->scan_stage = 0;
}

static void start_scan(cyw43_t *w, const uint8_t *opts, size_t len)
{
    w->scan_ssid_len = 0;
    if (len >= 44) {
        w->scan_sync = rd16(opts + 6);
        uint32_t n = rd32(opts + 8);
        if (n > 0 && n <= 32) {
            memcpy(w->scan_ssid, opts + 12, n);
            w->scan_ssid_len = n;
        }
    }
    w->scan_stage = 1;
    ev_schedule(w->m, &w->scan_ev, w->m->now + 300 * PS_PER_MS);
}

static void set_link(cyw43_t *w, bool up)
{
    if (w->link == up)
        return;
    w->link = up;
    pcsim_net_link(up);
}

static void leave(cyw43_t *w)
{
    ev_cancel(w->m, &w->join_ev);
    if (!w->joined)
        return;
    const ap_t *a = find_ap(w, w->join_ssid, w->join_ssid_len);
    const uint8_t *bssid = a ? a->bssid : NULL;
    w->joined = 0;
    set_link(w, false);
    send_event(w, EV_LINK, ST_SUCCESS, 0, 0, bssid, NULL, 0);
    send_event(w, EV_DISASSOC, ST_SUCCESS, 0, 0, bssid, NULL, 0);
}

static void join_event(chip_t *m, emu_event_t *ev)
{
    (void)m;
    cyw43_t *w = ev->ctx;
    if (!w->powered)
        return;
    bool secured = w->wsec != 0 || w->wpa_auth != 0;
    ap_t *a = find_ap(w, w->join_ssid, w->join_ssid_len);
    if (!a && w->permissive) {
        add_ap(w, (const char *)w->join_ssid, w->join_ssid_len, secured ? "" : NULL, true);
        a = find_ap(w, w->join_ssid, w->join_ssid_len);
    }
    if (!a) {
        if (w->missing_len != w->join_ssid_len || memcmp(w->missing, w->join_ssid, w->join_ssid_len)) {
            pcsim_log("wifi: no access point named '%.*s'", (int)w->join_ssid_len, w->join_ssid);
            memcpy(w->missing, w->join_ssid, w->join_ssid_len);
            w->missing_len = w->join_ssid_len;
        }
        send_event(w, EV_SET_SSID, ST_NO_NETWORKS, 0, 0, NULL, NULL, 0);
        return;
    }
    if (a->any_pass)
        a->secured = secured;
    if (a->secured != secured) {
        pcsim_log("wifi: '%s' is %s but the firmware asked for %s", a->ssid, a->secured ? "secured" : "open",
                  secured ? "a secured join" : "an open join");
        send_event(w, EV_SET_SSID, ST_FAIL, 0, 0, a->bssid, NULL, 0);
        return;
    }
    send_event(w, EV_AUTH, ST_SUCCESS, 0, 0, a->bssid, NULL, 0);
    send_event(w, EV_ASSOC, ST_SUCCESS, 0, 0, a->bssid, NULL, 0);
    send_event(w, EV_LINK, ST_SUCCESS, 0, 1, a->bssid, NULL, 0);
    if (secured && !a->any_pass && strcmp(w->key, a->pass) != 0) {
        pcsim_log("wifi: wrong password for '%s'", a->ssid);
        send_event(w, EV_PSK_SUP, WLC_SUP_KEYXCHANGE_FAIL, WLC_E_SUP_DEAUTH, 0, a->bssid, NULL, 0);
        return;
    }
    if (secured)
        send_event(w, EV_PSK_SUP, WLC_SUP_KEYED, 0, 0, a->bssid, NULL, 0);
    send_event(w, EV_SET_SSID, ST_SUCCESS, 0, 0, a->bssid, NULL, 0);
    w->joined = 1;
    set_link(w, true);
    pcsim_log("wifi: joined '%s'", a->ssid);
}

static void start_join(cyw43_t *w, const uint8_t *ssid, uint32_t len)
{
    if (len > 32)
        len = 32;
    leave(w);
    memcpy(w->join_ssid, ssid, len);
    w->join_ssid_len = len;
    // A network that isn't there takes a scan of every channel to give up on.
    bool known = w->permissive || find_ap(w, ssid, len);
    ev_schedule(w->m, &w->join_ev, w->m->now + (known ? 200 : 1500) * PS_PER_MS);
}

// ---------------------------------------------------------------------------------------------
// Ioctls and iovars

static var_t *find_var(cyw43_t *w, const char *name, bool create)
{
    for (int i = 0; i < w->nvars; i++)
        if (!strcmp(w->vars[i].name, name))
            return &w->vars[i];
    if (!create || w->nvars >= (int)(sizeof(w->vars) / sizeof(w->vars[0])) || strlen(name) >= sizeof(w->vars[0].name))
        return NULL;
    var_t *v = &w->vars[w->nvars++];
    snprintf(v->name, sizeof(v->name), "%s", name);
    v->len = 0;
    return v;
}

static void put_string(uint8_t *buf, size_t len, const char *s)
{
    size_t n = strlen(s) + 1;
    memcpy(buf, s, n < len ? n : len);
}

static int32_t get_var(cyw43_t *w, const char *name, uint8_t *buf, size_t len)
{
    uint8_t tmp[BUF_MAX];
    memset(tmp, 0, sizeof(tmp));
    if (!strcmp(name, "cur_etheraddr")) {
        memcpy(tmp, w->mac, 6);
    } else if (!strcmp(name, "clmver")) {
        put_string(tmp, sizeof(tmp), "API: 12.2 Data: pcsim Compiler: none ClmImport: none Creation: none");
    } else if (!strcmp(name, "ver")) {
        put_string(tmp, sizeof(tmp), "wl0: pcsim CYW43439 model 7.95.61 FWID 01-pcsim\n");
    } else if (!strcmp(name, "mcast_list")) {
        memcpy(tmp, w->mcast, sizeof(w->mcast));
    } else if (!strcmp(name, "ccgpioin") || !strcmp(name, "bss") || !strcmp(name, "maxassoc") ||
               !strcmp(name, "clmload_status")) {
        wr32(tmp, 0);
    } else {
        var_t *v = find_var(w, name, false);
        if (v)
            memcpy(tmp, v->val, v->len);
    }
    memcpy(buf, tmp, len < sizeof(tmp) ? len : sizeof(tmp));
    return 0;
}

static int32_t set_var(cyw43_t *w, const char *name, const uint8_t *val, size_t len)
{
    if (!strcmp(name, "escan")) {
        start_scan(w, val, len);
    } else if (!strcmp(name, "join")) {
        if (len >= 36)
            start_join(w, val + 4, rd32(val));
    } else if (!strcmp(name, "cur_etheraddr")) {
        if (len >= 6)
            memcpy(w->mac, val, 6);
    } else if (!strcmp(name, "gpioout")) {
        if (len >= 8 && (rd32(val) & 1)) {
            bool on = rd32(val + 4) & 1;
            if (on != w->led && w->m->trace)
                pcsim_log("wifi: LED %s", on ? "on" : "off");
            w->led = on;
        }
    } else if (!strcmp(name, "bsscfg:event_msgs")) {
        if (len >= 4 + sizeof(w->evmask))
            memcpy(w->evmask, val + 4, sizeof(w->evmask));
    } else if (!strcmp(name, "mcast_list")) {
        memcpy(w->mcast, val, len < sizeof(w->mcast) ? len : sizeof(w->mcast));
    } else if (!strcmp(name, "sae_password")) {
        if (len >= 2) {
            size_t n = rd16(val);
            if (n > len - 2)
                n = len - 2;
            if (n >= sizeof(w->key))
                n = sizeof(w->key) - 1;
            memcpy(w->key, val + 2, n);
            w->key[n] = 0;
        }
    } else if (strcmp(name, "clmload") != 0) {
        var_t *v = find_var(w, name, true);
        if (v) {
            v->len = len < sizeof(v->val) ? len : sizeof(v->val);
            memcpy(v->val, val, v->len);
        }
    }
    return 0;
}

static int32_t do_ioctl(cyw43_t *w, uint32_t cmd, bool set, uint8_t *buf, size_t len, size_t in_len)
{
    switch (cmd) {
    case WLC_UP: w->up = true; return 0;
    case WLC_DOWN:
        leave(w);
        w->up = false;
        return 0;
    case WLC_SET_WSEC:
        if (in_len >= 4)
            w->wsec = rd32(buf);
        return 0;
    case WLC_SET_WPA_AUTH:
        if (in_len >= 4)
            w->wpa_auth = rd32(buf);
        return 0;
    case WLC_SET_PM:
        if (in_len >= 4)
            w->pm = rd32(buf);
        return 0;
    case WLC_GET_PM:
        if (len >= 4)
            wr32(buf, w->pm);
        return 0;
    case WLC_SET_WSEC_PMK:
        if (in_len >= 4) {
            size_t n = rd16(buf);
            if (n > in_len - 4)
                n = in_len - 4;
            if (n >= sizeof(w->key))
                n = sizeof(w->key) - 1;
            memcpy(w->key, buf + 4, n);
            w->key[n] = 0;
        }
        return 0;
    case WLC_SET_SSID:
        if (in_len >= 4)
            start_join(w, buf + 4, rd32(buf));
        return 0;
    case WLC_DISASSOC: leave(w); return 0;
    case WLC_GET_BSSID: {
        const ap_t *a = w->joined ? find_ap(w, w->join_ssid, w->join_ssid_len) : NULL;
        memset(buf, 0, len);
        if (a && len >= 6)
            memcpy(buf, a->bssid, 6);
        return a ? 0 : -17;
    }
    case WLC_GET_SSID:
        memset(buf, 0, len);
        if (w->joined && len >= 36) {
            wr32(buf, w->join_ssid_len);
            memcpy(buf + 4, w->join_ssid, w->join_ssid_len);
        }
        return 0;
    case WLC_GET_CHANNEL: {
        const ap_t *a = w->joined ? find_ap(w, w->join_ssid, w->join_ssid_len) : NULL;
        memset(buf, 0, len);
        if (len >= 12) {
            wr32(buf, a ? a->channel : 1);
            wr32(buf + 4, a ? a->channel : 1);
        }
        return 0;
    }
    case WLC_GET_RSSI: {
        const ap_t *a = w->joined ? find_ap(w, w->join_ssid, w->join_ssid_len) : NULL;
        if (len >= 4)
            wr32(buf, (uint32_t)(int32_t)(a ? a->rssi : 0));
        return 0;
    }
    case WLC_GET_ASSOCLIST:
        if (len >= 4)
            wr32(buf, 0);
        return 0;
    case WLC_GET_VAR:
    case WLC_SET_VAR: {
        size_t n = strnlen((const char *)buf, len);
        if (n == len)
            return -2;
        char name[64];
        snprintf(name, sizeof(name), "%s", (const char *)buf);
        if (cmd == WLC_GET_VAR)
            return get_var(w, name, buf, len);
        return set_var(w, name, buf + n + 1, in_len > n + 1 ? in_len - n - 1 : 0);
    }
    default:
        (void)set;
        return 0;
    }
}

static void control_from_host(cyw43_t *w, const uint8_t *p, size_t n)
{
    if (n < 16)
        return;
    uint32_t cmd = rd32(p), len = rd32(p + 4) & 0xffff, flags = rd32(p + 8);
    size_t in_len = n - 16 < len ? n - 16 : len;
    if (12 + 16 + len > PKT_MAX)
        len = PKT_MAX - 12 - 16;
    uint8_t buf[PKT_MAX];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, p + 16, in_len);
    int32_t status = do_ioctl(w, cmd, (flags & 2) != 0, buf, len, in_len);
    if (debug)
        pcsim_log("wifi: ioctl %u%s%s len %u -> %d", cmd, cmd == WLC_GET_VAR || cmd == WLC_SET_VAR ? " " : "",
                  cmd == WLC_GET_VAR || cmd == WLC_SET_VAR ? (const char *)p + 16 : "", len, status);
    pkt_t *r = pkt_new(w, CH_CONTROL, 16 + len);
    wr32(r->buf + 12, cmd);
    wr32(r->buf + 16, len);
    wr32(r->buf + 20, flags);
    wr32(r->buf + 24, (uint32_t)status);
    memcpy(r->buf + 28, buf, len);
    pkt_push(w, r);
}

static void sdpcm_from_host(cyw43_t *w, const uint8_t *d, size_t len)
{
    if (len < 12)
        return;
    uint16_t size = rd16(d), com = rd16(d + 2);
    if ((size ^ com) != 0xffff || size < 12 || size > len)
        return;
    w->host_seq = (uint8_t)(d[4] + 1);
    unsigned chan = d[5] & 15, hlen = d[7];
    if (hlen < 12 || hlen > size)
        return;
    if (chan == CH_CONTROL) {
        control_from_host(w, d + hlen, size - hlen);
    } else if (chan == CH_DATA && size >= hlen + 4) {
        size_t off = hlen + 4 + (size_t)d[hlen + 3] * 4;
        if (off < size && w->link)
            pcsim_net_input(d + off, size - off);
    }
    maybe_credit(w);
}

// ---------------------------------------------------------------------------------------------
// Backplane

static uint32_t bp_reg_read(cyw43_t *w, uint32_t a)
{
    switch (a) {
    case 0x18000000u: return 0x1545a9afu;
    case 0x18002020u: return w->sdio_int;
    case 0x18002024u: return w->sdio_host_mask;
    case 0x18002034u: return w->sdio_func_mask;
    case 0x18004010u: return w->bankx_index;
    case 0x18004044u: return w->bankx_pda;
    case ARM_WRAPPER + AI_IOCTRL: return w->arm_ioctrl;
    case ARM_WRAPPER + AI_RESETCTRL: return w->arm_reset;
    case SRAM_WRAPPER + AI_IOCTRL: return w->sram_ioctrl;
    case SRAM_WRAPPER + AI_RESETCTRL: return w->sram_reset;
    default: return 0;
    }
}

static void boot_done(chip_t *m, emu_event_t *ev)
{
    (void)m;
    cyw43_t *w = ev->ctx;
    w->booted = true;
    if (w->m->trace) {
        const char *ver = NULL;
        for (size_t i = 0; i + 9 < RAM_SIZE; i++)
            if (w->ram[i] == 'V' && !memcmp(w->ram + i, "Version: ", 9)) {
                ver = (const char *)w->ram + i;
                break;
            }
        pcsim_log("wifi: radio firmware downloaded%s%.60s", ver ? ": " : "", ver ? ver : "");
    }
    update_irq(w);
}

static void bp_reg_write(cyw43_t *w, uint32_t a, uint32_t v)
{
    switch (a) {
    case 0x18002020u: w->sdio_int &= ~v; break;
    case 0x18002024u: w->sdio_host_mask = v; break;
    case 0x18002034u: w->sdio_func_mask = v; break;
    case 0x18004010u: w->bankx_index = v; break;
    case 0x18004044u: w->bankx_pda = v; break;
    case SRAM_WRAPPER + AI_IOCTRL: w->sram_ioctrl = (uint8_t)v; break;
    case SRAM_WRAPPER + AI_RESETCTRL: w->sram_reset = (uint8_t)(v & 1); break;
    case ARM_WRAPPER + AI_RESETCTRL: w->arm_reset = (uint8_t)(v & 1); break;
    case ARM_WRAPPER + AI_IOCTRL:
        w->arm_ioctrl = (uint8_t)v;
        // Released from reset with its clock running and not halted: the radio firmware boots.
        if (!w->arm_reset && (v & SICF_CLOCK_EN) && !(v & (SICF_FGC | SICF_CPUHALT)) && !w->booted &&
            !w->boot_ev.queued) {
            w->boot_ev.fn = boot_done;
            w->boot_ev.ctx = w;
            ev_schedule(w->m, &w->boot_ev, w->m->now + 25 * PS_PER_MS);
        }
        break;
    default: break;
    }
}

static void bp_read(cyw43_t *w, uint32_t a, uint8_t *dst, size_t len)
{
    for (size_t i = 0; i < len; i++, a++) {
        if (a < RAM_SIZE)
            dst[i] = w->ram[a];
        else
            dst[i] = (uint8_t)(bp_reg_read(w, a & ~3u) >> (8 * (a & 3)));
    }
}

static void bp_write(cyw43_t *w, uint32_t a, const uint8_t *src, size_t len)
{
    if (a < RAM_SIZE) {
        size_t n = a + len <= RAM_SIZE ? len : RAM_SIZE - a;
        memcpy(w->ram + a, src, n);
        return;
    }
    // Registers are written whole; a byte access leaves the register's other bytes as they were.
    uint32_t base = a & ~3u;
    uint32_t v = bp_reg_read(w, base);
    for (size_t i = 0; i < len && i < 4; i++) {
        unsigned sh = 8 * ((a + i) & 3);
        v = (v & ~(0xffu << sh)) | (uint32_t)src[i] << sh;
    }
    bp_reg_write(w, base, v);
}

static uint8_t f1_reg_read(cyw43_t *w, uint32_t a)
{
    switch (a) {
    case F1_WATERMARK: return w->watermark;
    case F1_ADDR_LOW: return (uint8_t)(w->window >> 8);
    case F1_ADDR_MID: return (uint8_t)(w->window >> 16);
    case F1_ADDR_HIGH: return (uint8_t)(w->window >> 24);
    case F1_CLOCK_CSR:
        return (uint8_t)((w->clock_csr & 0x3f) | CSR_ALP_AVAIL | (w->booted ? CSR_HT_AVAIL : 0));
    case F1_PULL_UP: return w->pull_up;
    case F1_WAKEUP: return w->wakeup;
    case F1_SLEEP_CSR: return (uint8_t)(w->sleep_csr | ((w->sleep_csr & 1) ? 2 : 0));
    default: return 0;
    }
}

static void f1_reg_write(cyw43_t *w, uint32_t a, uint8_t v)
{
    switch (a) {
    case F1_WATERMARK: w->watermark = v; break;
    case F1_ADDR_LOW: w->window = (w->window & 0xffff00ffu) | (uint32_t)v << 8; break;
    case F1_ADDR_MID: w->window = (w->window & 0xff00ffffu) | (uint32_t)v << 16; break;
    case F1_ADDR_HIGH: w->window = (w->window & 0x00ffffffu) | (uint32_t)v << 24; break;
    case F1_FRAME_CTRL:
        if (v & 1)
            pkt_pop(w);
        break;
    case F1_CLOCK_CSR: w->clock_csr = v; break;
    case F1_PULL_UP: w->pull_up = v; break;
    case F1_WAKEUP: w->wakeup = v; break;
    case F1_SLEEP_CSR: w->sleep_csr = v & 1; break;
    default: break;
    }
}

// ---------------------------------------------------------------------------------------------
// gSPI

static uint16_t int_status(cyw43_t *w)
{
    return w->qh ? INT_F2_PACKET : 0;
}

static uint32_t status_reg(cyw43_t *w)
{
    uint32_t s = w->booted ? STATUS_F2_RX_READY : 0;
    if (w->qh)
        s |= STATUS_F2_PKT | 0x08 | (uint32_t)(w->qh->len & 0x7ff) << 9;
    return s;
}

static uint8_t f0_byte(cyw43_t *w, uint32_t a)
{
    static const uint8_t test[4] = {0xad, 0xbe, 0xed, 0xfe};
    switch (a) {
    case 0x04: return (uint8_t)int_status(w);
    case 0x05: return (uint8_t)(int_status(w) >> 8);
    case 0x08:
    case 0x09:
    case 0x0a:
    case 0x0b: return (uint8_t)(status_reg(w) >> (8 * (a - 8)));
    case 0x0c: return 3;
    case 0x0e: return w->booted ? 3 : 1;
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17: return test[a - 0x14];
    default: return a < sizeof(w->f0) ? w->f0[a] : 0;
    }
}

static void f0_write(cyw43_t *w, uint32_t a, const uint8_t *d, size_t len)
{
    for (size_t i = 0; i < len; i++, a++) {
        if ((a >= 0x04 && a <= 0x05) || (a >= 0x08 && a <= 0x17) || a >= sizeof(w->f0))
            continue;
        w->f0[a] = d[i];
    }
    if (w->f0[0] & BUS_WORD32)
        w->word32 = true;
    update_irq(w);
}

static void prepare_read(cyw43_t *w, unsigned fn, uint32_t addr, size_t len)
{
    size_t pad = fn == 1 ? w->f0[0x1d] : 0;
    size_t total = pad + ((len + 3) & ~3u);
    if (total > sizeof(w->out))
        total = sizeof(w->out);
    memset(w->out, 0, total);
    uint8_t *d = w->out + pad;
    size_t n = total - pad < len ? total - pad : len;
    if (fn == 0) {
        for (size_t i = 0; i < n; i++)
            d[i] = f0_byte(w, addr + (uint32_t)i);
    } else if (fn == 1) {
        if (addr >= 0x10000) {
            for (size_t i = 0; i < n; i++)
                d[i] = f1_reg_read(w, addr + (uint32_t)i);
        } else {
            bp_read(w, (w->window & ~0x7fffu) | (addr & 0x7fff), d, n);
        }
    } else if (fn == 2) {
        pkt_t *p = w->qh;
        if (p) {
            size_t c = p->len < n ? p->len : n;
            if (debug)
                pcsim_log("wifi: host reads packet chan %u len %zu (asked %zu)", p->buf[5], p->len, n);
            memcpy(d, p->buf, c);
            w->credit = (uint8_t)(w->host_seq + CREDIT_WINDOW);
            if (c > 9)
                d[9] = w->credit;
            pkt_pop(w);
        }
    }
    if (!w->word32)
        swap16(w->out, total);
    w->out_len = total;
    w->out_pos = 0;
    w->out_bit = 0;
}

static void finish_write(cyw43_t *w)
{
    if (w->in_len < 4 || !(w->cmd >> 31))
        return;
    unsigned fn = (w->cmd >> 28) & 3;
    uint32_t addr = (w->cmd >> 11) & 0x1ffff;
    size_t len = w->cmd & 0x7ff;
    if (!len)
        len = BUF_MAX;
    uint8_t *d = w->in + 4;
    size_t have = w->in_len - 4;
    if (!w->word32)
        swap16(d, have);
    if (len > have)
        len = have;
    if (fn == 0) {
        f0_write(w, addr, d, len);
    } else if (fn == 1) {
        if (addr >= 0x10000) {
            for (size_t i = 0; i < len; i++)
                f1_reg_write(w, addr + (uint32_t)i, d[i]);
        } else {
            bp_write(w, (w->window & ~0x7fffu) | (addr & 0x7fff), d, len);
        }
    } else if (fn == 2) {
        sdpcm_from_host(w, d, len);
    }
}

static void byte_in(cyw43_t *w, uint8_t b)
{
    if (w->in_len < sizeof(w->in))
        w->in[w->in_len++] = b;
    if (w->in_len != 4)
        return;
    uint8_t c[4];
    memcpy(c, w->in, 4);
    if (!w->word32)
        swap16(c, 4);
    w->cmd = rd32(c);
    if (debug)
        pcsim_log("wifi: bus %s fn %u addr %05x len %u", (w->cmd >> 31) ? "write" : "read", (w->cmd >> 28) & 3,
                  (w->cmd >> 11) & 0x1ffff, w->cmd & 0x7ff);
    if (!(w->cmd >> 31))
        prepare_read(w, (w->cmd >> 28) & 3, (w->cmd >> 11) & 0x1ffff, w->cmd & 0x7ff);
}

bool cyw43_bus_pins(chip_t *m, unsigned clk, unsigned data)
{
    return m->wifi && clk == PIN_CLK && data == PIN_DATA;
}

void cyw43_bus_out(chip_t *m, int bit)
{
    cyw43_t *w = m->wifi;
    if (!w || !w->powered || !w->selected)
        return;
    w->in_byte = (uint8_t)((w->in_byte << 1) | (bit & 1));
    if (++w->in_bits == 8) {
        w->in_bits = 0;
        byte_in(w, w->in_byte);
    }
}

int cyw43_bus_in(chip_t *m)
{
    cyw43_t *w = m->wifi;
    if (!w || !w->powered || !w->selected)
        return 0;
    if (w->out_bit == 0)
        w->out_cur = w->out_pos < w->out_len ? w->out[w->out_pos] : 0;
    int bit = (w->out_cur >> (7 - w->out_bit)) & 1;
    if (++w->out_bit == 8) {
        w->out_bit = 0;
        w->out_pos++;
    }
    return bit;
}

// With CS high the data pin doubles as the host interrupt, once the host has set its polarity.
static void update_irq(cyw43_t *w)
{
    chip_t *m = w->m;
    bool drive = w->powered && !w->selected && w->word32;
    bool active = (int_status(w) & rd16(w->f0 + 6)) != 0;
    bool level = (w->f0[0] & BUS_IRQ_HIGH) ? active : !active;
    gpio_pin_t *g = &m->gpio[PIN_DATA];
    if (g->ext_driven == drive && (!drive || g->ext_level == level))
        return;
    if (debug)
        pcsim_log("wifi: irq pin %s%s", drive ? "driven " : "released", drive ? (level ? "high" : "low") : "");
    g->ext_driven = drive;
    g->ext_level = drive && level;
    gpio_refresh_mask(m, 1ull << PIN_DATA);
}

static void poll_event(chip_t *m, emu_event_t *ev)
{
    cyw43_t *w = ev->ctx;
    if (!w->powered)
        return;
    pcsim_net_poll();
    ev_schedule(m, &w->poll_ev, m->now + POLL_PS);
}

static void power(cyw43_t *w, bool on)
{
    chip_t *m = w->m;
    ev_cancel(m, &w->boot_ev);
    ev_cancel(m, &w->scan_ev);
    ev_cancel(m, &w->join_ev);
    ev_cancel(m, &w->poll_ev);
    queue_flush(w);
    set_link(w, false);
    uint8_t *ram = w->ram;
    uint8_t mac[6];
    memcpy(mac, w->mac, 6);
    memset(&w->word32, 0, sizeof(*w) - offsetof(cyw43_t, word32));
    w->ram = ram;
    memcpy(w->mac, mac, 6);
    w->arm_reset = w->sram_reset = 1;
    w->credit = 1;
    memset(w->evmask, 0xff, sizeof(w->evmask));
    w->boot_ev = (emu_event_t){.fn = boot_done, .ctx = w};
    w->scan_ev = (emu_event_t){.fn = scan_event, .ctx = w};
    w->join_ev = (emu_event_t){.fn = join_event, .ctx = w};
    w->poll_ev = (emu_event_t){.fn = poll_event, .ctx = w};
    config_aps(w);
    w->powered = on;
    w->selected = on && !m->gpio[PIN_CS].level;
    if (on)
        ev_schedule(m, &w->poll_ev, m->now + POLL_PS);
    update_irq(w);
}

static void reg_on_changed(void *ctx, unsigned pin, bool level)
{
    (void)pin;
    cyw43_t *w = ctx;
    if (level != w->powered)
        power(w, level);
}

static void cs_changed(void *ctx, unsigned pin, bool level)
{
    (void)pin;
    cyw43_t *w = ctx;
    if (!w->powered)
        return;
    bool sel = !level;
    if (sel == w->selected)
        return;
    if (!sel)
        finish_write(w);
    w->selected = sel;
    w->in_len = 0;
    w->in_bits = 0;
    w->cmd = 0;
    w->out_len = w->out_pos = 0;
    w->out_bit = 0;
    update_irq(w);
}

void cyw43_init(chip_t *m)
{
    debug = getenv("PCSIM_WIFI_DEBUG") != NULL;
    cyw43_t *w = calloc(1, sizeof(*w));
    w->m = m;
    w->ram = calloc(1, RAM_SIZE);
    // A Raspberry Pi OUI, with the rest taken from the emulated chip's ID so it stays the same.
    static const uint8_t mac[6] = {0x28, 0xcd, 0xc1, 0x5a, 0x8f, 0x3b};
    memcpy(w->mac, mac, 6);
    m->wifi = w;
    power(w, false);
    pcsim_net_attach(rx_frame, w);
    pcsim_gpio_watch(PIN_REG_ON, reg_on_changed, w);
    pcsim_gpio_watch(PIN_CS, cs_changed, w);
}

void cyw43_free(chip_t *m)
{
    cyw43_t *w = m->wifi;
    if (!w)
        return;
    power(w, false);
    pcsim_net_attach(NULL, NULL);
    free(w->ram);
    free(w);
    m->wifi = NULL;
}

void cyw43_reset(chip_t *m)
{
    if (m->wifi)
        power(m->wifi, false);
}
