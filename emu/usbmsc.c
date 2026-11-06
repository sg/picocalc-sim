#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usbhost.h"

// The host's mass storage driver: SCSI commands over the bulk-only transport, each a command block
// wrapper out, data in or out, and a status wrapper back. A drive is mounted as an OS would: the
// host asks what it is, waits for it to be ready, reads its capacity and the FAT volume on it, and
// logs its label and INFO_UF2.TXT. A UF2 copied onto it goes as an OS's file copy would to the
// device, 64 KB writes of the file's 512-byte blocks, which a boot ROM's drive looks at one by one.

enum { PH_IDLE, PH_CBW, PH_DATA, PH_CSW, PH_CLEAR, PH_PAUSE };
enum { M_MAX_LUN, M_INQUIRY, M_READY, M_SENSE, M_CAPACITY, M_SECTOR0, M_BOOT, M_ROOT, M_INFO, M_MOUNTED, M_COPY,
       M_COPIED };
// A command that never got a status wrapper back.
enum { CSW_PASSED = 0, CSW_FAILED = 1, CSW_PHASE = 2, CSW_NONE = -1 };

#define COPY_CHUNK (128 * 512)

static void next(usb_host_t *h);

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static unsigned le16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

static void copy_text(char *out, size_t cap, const uint8_t *p, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++)
        out[o++] = p[i] >= 0x20 && p[i] < 0x7f ? (char)p[i] : ' ';
    while (o && out[o - 1] == ' ')
        o--;
    out[o] = 0;
}

// INFO_UF2.TXT's lines, joined with semicolons.
static void join_lines(char *out, size_t cap, const uint8_t *p, size_t n)
{
    size_t o = 0;
    bool sep = false;
    for (size_t i = 0; i < n && o + 3 < cap; i++) {
        uint8_t c = p[i];
        if (c == '\r' || c == '\n') {
            sep = o > 0;
            continue;
        }
        if (c < 0x20 || c >= 0x7f)
            continue;
        if (sep) {
            out[o++] = ';';
            out[o++] = ' ';
            sep = false;
        }
        out[o++] = (char)c;
    }
    out[o] = 0;
}

static void schedule(usb_host_t *h, uint64_t after)
{
    ev_schedule(h->m, &h->msc.ev, h->m->now + after);
}

static void finish(usb_host_t *h, int status)
{
    msc_t *d = &h->msc;
    d->phase = PH_IDLE;
    d->done(h, status);
}

static void command(usb_host_t *h, const uint8_t *cb, unsigned cb_len, bool in, uint8_t *data, unsigned len,
                    void (*done)(usb_host_t *, int))
{
    msc_t *d = &h->msc;
    uint8_t *w = d->cbw;
    memset(w, 0, sizeof(d->cbw));
    wr32le(w, 0x43425355u);
    wr32le(w + 4, ++d->tag);
    wr32le(w + 8, len);
    w[12] = in ? 0x80 : 0;
    w[14] = (uint8_t)cb_len;
    memcpy(w + 15, cb, cb_len);
    d->data = data;
    d->len = len;
    d->moved = 0;
    d->in = in;
    d->done = done;
    d->naks = d->errors = 0;
    d->csw_retried = false;
    d->phase = PH_CBW;
    // A 64 KB write waits on the flash erasing and programming behind it, which takes seconds.
    d->deadline = h->m->now + 20 * PS_PER_S;
    schedule(h, usbhost_bus_time(0));
}

static void rw10(usb_host_t *h, bool write, uint32_t lba, unsigned count, uint8_t *data,
                 void (*done)(usb_host_t *, int))
{
    uint8_t cb[10] = {write ? 0x2a : 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8),
                      (uint8_t)lba, 0, (uint8_t)(count >> 8), (uint8_t)count, 0};
    command(h, cb, sizeof(cb), !write, data, count * 512, done);
}

static void halt_cleared(usb_host_t *h, int r)
{
    msc_t *d = &h->msc;
    if (r != XF_OK) {
        finish(h, CSW_NONE);
        return;
    }
    // An endpoint whose halt is cleared starts again on DATA0.
    if (d->halt_ep & 0x80)
        d->in_toggle = 0;
    else
        d->out_toggle = 0;
    d->phase = PH_CSW;
    schedule(h, usbhost_bus_time(0));
}

// One transaction of the command, then the next after the bus time it took.
static void xact(chip_t *m, emu_event_t *ev)
{
    usb_host_t *h = ev->ctx;
    msc_t *d = &h->msc;
    uint8_t buf[1023];
    unsigned n = 0, pid = 0;
    int r = USB_NONE;
    uint64_t t = usbhost_bus_time(0);
    if (d->phase == PH_PAUSE) {
        d->phase = PH_IDLE;
        next(h);
        return;
    }
    if (d->phase == PH_IDLE)
        return;
    if (m->now > d->deadline) {
        finish(h, CSW_NONE);
        return;
    }
    switch (d->phase) {
    case PH_CBW:
        r = usb_bus_out(m, h->addr, d->out_ep & 15, d->out_toggle, d->cbw, sizeof(d->cbw));
        t = usbhost_bus_time(sizeof(d->cbw));
        if (r == USB_ACK) {
            d->out_toggle ^= 1;
            d->phase = d->len ? PH_DATA : PH_CSW;
        }
        break;
    case PH_DATA:
        if (d->in) {
            r = usb_bus_in(m, h->addr, d->in_ep & 15, buf, &n, &pid);
            t = usbhost_bus_time(n);
            // A packet with the toggle already seen repeats one whose ACK went astray.
            if (r == USB_ACK && pid == d->in_toggle) {
                d->in_toggle ^= 1;
                unsigned take = n < d->len - d->moved ? n : d->len - d->moved;
                memcpy(d->data + d->moved, buf, take);
                d->moved += take;
                if (n < d->in_mps || d->moved >= d->len)
                    d->phase = PH_CSW;
            }
        } else {
            n = d->len - d->moved < d->out_mps ? d->len - d->moved : d->out_mps;
            r = usb_bus_out(m, h->addr, d->out_ep & 15, d->out_toggle, d->data + d->moved, n);
            t = usbhost_bus_time(n);
            if (r == USB_ACK) {
                d->out_toggle ^= 1;
                d->moved += n;
                if (d->moved >= d->len)
                    d->phase = PH_CSW;
            }
        }
        break;
    case PH_CSW:
        r = usb_bus_in(m, h->addr, d->in_ep & 15, buf, &n, &pid);
        t = usbhost_bus_time(n);
        if (r == USB_ACK && pid == d->in_toggle) {
            d->in_toggle ^= 1;
            bool valid = n == 13 && rd32le(buf) == 0x53425355u && rd32le(buf + 4) == d->tag;
            finish(h, valid ? buf[12] : CSW_NONE);
            return;
        }
        break;
    case PH_CLEAR:
        if (!usbhost_control(h, 0x02, 1, 0, d->halt_ep, 0, halt_cleared))
            schedule(h, PS_PER_MS);
        return;
    default: return;
    }
    switch (r) {
    case USB_ACK:
        d->naks = 0;
        d->errors = 0;
        break;
    case USB_NAK: t = usbhost_nak_delay(++d->naks); break;
    case USB_STALL:
        // A stalled data stage ends early, and its status follows once the halt is cleared; a
        // stalled status is cleared and read once more. A refused command block fails the command.
        if (d->phase == PH_CBW || (d->phase == PH_CSW && d->csw_retried)) {
            finish(h, CSW_NONE);
            return;
        }
        if (d->phase == PH_CSW)
            d->csw_retried = true;
        d->halt_ep = d->phase == PH_DATA && !d->in ? d->out_ep : d->in_ep;
        d->phase = PH_CLEAR;
        t = usbhost_bus_time(0);
        break;
    default:
        if (++d->errors >= 3) {
            finish(h, CSW_NONE);
            return;
        }
        break;
    }
    schedule(h, t);
}

static void give_up(usb_host_t *h, const char *why)
{
    msc_t *d = &h->msc;
    usbhost_log(h, "the drive did not mount: %s", why);
    d->failed = true;
    if (d->copy) {
        usbhost_log(h, "%s was not copied", d->copy_name);
        free(d->copy);
        d->copy = NULL;
    }
}

static void step_done(usb_host_t *h, int status)
{
    msc_t *d = &h->msc;
    static const char *const what[] = {"", "INQUIRY", "TEST UNIT READY", "REQUEST SENSE", "READ CAPACITY",
                                       "reading sector 0", "reading the boot sector", "reading the root directory",
                                       "reading INFO_UF2.TXT", "", "writing the UF2"};
    if (h->debug)
        usbhost_log(h, "drive step %d: status %d", d->step, status);
    // A drive that is not ready yet says why through REQUEST SENSE, then is asked again.
    if (d->step == M_READY && status != CSW_PASSED && status != CSW_NONE) {
        d->step = M_SENSE;
        next(h);
        return;
    }
    if (status != CSW_PASSED) {
        if (d->step == M_COPY) {
            usbhost_log(h, "the drive failed writing %s at %zu KB", d->copy_name, d->copy_off / 1024);
            free(d->copy);
            d->copy = NULL;
            d->step = M_MOUNTED;
            return;
        }
        if (d->step != M_SENSE) {
            give_up(h, what[d->step]);
            return;
        }
    }
    const uint8_t *b = d->buf;
    switch (d->step) {
    case M_READY:
        d->step = M_CAPACITY;
        next(h);
        return;
    case M_INQUIRY:
        copy_text(d->vendor, sizeof(d->vendor), b + 8, 8);
        copy_text(d->product, sizeof(d->product), b + 16, 16);
        break;
    case M_SENSE:
        if (++d->tries > 20) {
            give_up(h, "it never became ready");
            return;
        }
        d->step = M_READY;
        d->phase = PH_PAUSE;
        schedule(h, 100 * PS_PER_MS);
        return;
    case M_CAPACITY:
        d->blocks = be32(b) + 1;
        d->block_size = be32(b + 4);
        if (d->block_size != 512) {
            give_up(h, "its blocks are not 512 bytes");
            return;
        }
        break;
    case M_SECTOR0:
        // A partition table, or else the volume starts at the first sector.
        d->part_lba = 0;
        if (b[510] == 0x55 && b[511] == 0xaa && b[0x1c2] != 0 && memcmp(b + 0x36, "FAT", 3) != 0) {
            d->part_lba = rd32le(b + 0x1c6);
            break;
        }
        // Sector 0 is the boot sector itself.
        d->step = M_BOOT;
        // fall through
    case M_BOOT: {
        unsigned bps = le16(b + 11), reserved = le16(b + 14), fats = b[16], roots = le16(b + 17),
                 per_fat = le16(b + 22);
        if (bps != 512 || !b[13] || !fats || !per_fat) {
            give_up(h, "it holds no FAT volume");
            return;
        }
        d->cluster_sectors = b[13];
        d->root_lba = d->part_lba + reserved + fats * per_fat;
        d->root_sectors = (roots * 32 + 511) / 512;
        d->data_lba = d->root_lba + d->root_sectors;
        if (d->root_sectors > MSC_BUF / 512)
            d->root_sectors = MSC_BUF / 512;
        copy_text(d->label, sizeof(d->label), b + 0x2b, 11);
        d->step = M_ROOT;
        next(h);
        return;
    }
    case M_ROOT:
        d->info[0] = 0;
        for (unsigned i = 0; i < d->root_sectors * 512; i += 32) {
            const uint8_t *e = b + i;
            if (!e[0])
                break;
            if (e[0] == 0xe5 || e[11] == 0x0f)
                continue;
            if (e[11] & 0x08)
                copy_text(d->label, sizeof(d->label), e, 11);
            else if (!memcmp(e, "INFO_UF2TXT", 11)) {
                unsigned cluster = le16(e + 26);
                uint32_t size = rd32le(e + 28);
                if (cluster >= 2) {
                    d->info_len = size < 512 ? size : 512;
                    d->step = M_INFO;
                    rw10(h, false, d->data_lba + (cluster - 2) * d->cluster_sectors, 1, d->buf, step_done);
                    return;
                }
            }
        }
        d->step = M_MOUNTED;
        next(h);
        return;
    case M_INFO: join_lines(d->info, sizeof(d->info), b, d->info_len); break;
    case M_COPY:
        d->copy_off += d->moved;
        if (d->copy_off < d->copy_len) {
            next(h);
            return;
        }
        usbhost_log(h, "copied %s to the drive: %zu KB in %.2f s", d->copy_name, d->copy_len / 1024,
                    (double)(h->m->now - d->copy_t0) / PS_PER_S);
        free(d->copy);
        d->copy = NULL;
        // A boot ROM restarts into what it was given; one that does not was given something it
        // does not take.
        d->step = M_COPIED;
        d->phase = PH_PAUSE;
        schedule(h, 3 * PS_PER_S);
        return;
    default: break;
    }
    d->step++;
    next(h);
}

static void max_lun_done(usb_host_t *h, int r)
{
    (void)r;
    h->msc.step = M_INQUIRY;
    next(h);
}

static void report(usb_host_t *h)
{
    msc_t *d = &h->msc;
    usbhost_log(h, "drive \"%s\" (%s %s, %u MB)%s%s%s", d->label, d->vendor, d->product,
                (unsigned)((uint64_t)d->blocks * 512 >> 20), d->info[0] ? ", INFO_UF2.TXT \"" : "", d->info,
                d->info[0] ? "\"" : "");
}

// Issues what mounting or copying needs next.
static void next(usb_host_t *h)
{
    msc_t *d = &h->msc;
    if (usbip_exported(h))
        return;
    static const uint8_t inquiry[6] = {0x12, 0, 0, 0, 36, 0}, ready[6] = {0}, sense[6] = {0x03, 0, 0, 0, 18, 0},
                         capacity[10] = {0x25};
    switch (d->step) {
    case M_MAX_LUN:
        if (!usbhost_control(h, 0xa1, 0xfe, 0, d->iface, 1, max_lun_done)) {
            d->phase = PH_PAUSE;
            schedule(h, PS_PER_MS);
        }
        return;
    case M_INQUIRY: command(h, inquiry, sizeof(inquiry), true, d->buf, 36, step_done); return;
    case M_READY: command(h, ready, sizeof(ready), false, NULL, 0, step_done); return;
    case M_SENSE: command(h, sense, sizeof(sense), true, d->buf, 18, step_done); return;
    case M_CAPACITY: command(h, capacity, sizeof(capacity), true, d->buf, 8, step_done); return;
    case M_SECTOR0: rw10(h, false, 0, 1, d->buf, step_done); return;
    case M_BOOT: rw10(h, false, d->part_lba, 1, d->buf, step_done); return;
    case M_ROOT: rw10(h, false, d->root_lba, d->root_sectors, d->buf, step_done); return;
    case M_MOUNTED:
        if (!d->mounted) {
            d->mounted = true;
            report(h);
        }
        if (!d->copy)
            return;
        d->step = M_COPY;
        d->copy_off = 0;
        d->copy_t0 = h->m->now;
        usbhost_log(h, "copying %s to the drive (%zu KB)", d->copy_name, d->copy_len / 1024);
        // fall through
    case M_COPY: {
        size_t left = d->copy_len - d->copy_off;
        unsigned n = (unsigned)((left < COPY_CHUNK ? left : COPY_CHUNK) / 512);
        // Into free clusters past the drive's own files, where an OS would put a new one.
        uint32_t lba = d->data_lba + 64 * d->cluster_sectors + (uint32_t)(d->copy_off / 512);
        rw10(h, true, lba, n, d->copy + d->copy_off, step_done);
        return;
    }
    case M_COPIED:
        usbhost_log(h, "the device has not restarted, so its boot ROM did not take the UF2: it may be for "
                       "another chip");
        d->step = M_MOUNTED;
        if (d->copy)
            next(h);
        return;
    default: return;
    }
}

void msc_init(usb_host_t *h)
{
    h->msc.ev.fn = xact;
    h->msc.ev.ctx = h;
}

// A mass storage interface (class 8) using SCSI (subclass 6) over the bulk-only transport (0x50).
void msc_configured(usb_host_t *h)
{
    msc_t *d = &h->msc;
    int cur = -1, alt = -1;
    if (usbip_exported(h))
        return;
    d->found = d->mounted = d->failed = false;
    d->in_ep = d->out_ep = 0;
    for (unsigned i = 0; i + 2 <= h->cfg_len && h->cfg[i] >= 2; i += h->cfg[i]) {
        const uint8_t *e = h->cfg + i;
        if (e[1] == 4 && e[0] >= 9) {
            cur = e[5] == 0x08 && e[6] == 0x06 && e[7] == 0x50 && !d->found ? e[2] : -1;
            alt = e[3];
            if (cur >= 0)
                d->iface = (uint8_t)cur;
        } else if (e[1] == 5 && e[0] >= 7 && cur >= 0 && alt == 0 && (e[3] & 3) == 2) {
            unsigned mps = (e[4] | e[5] << 8) & 0x7ff;
            if (e[2] & 0x80) {
                d->in_ep = e[2];
                d->in_mps = mps;
            } else {
                d->out_ep = e[2];
                d->out_mps = mps;
            }
            d->found = d->in_ep && d->out_ep;
        }
    }
    if (!d->found) {
        if (d->copy) {
            usbhost_log(h, "the device has no drive, so %s was not copied", d->copy_name);
            free(d->copy);
            d->copy = NULL;
        }
        return;
    }
    d->in_toggle = d->out_toggle = 0;
    d->tries = 0;
    d->step = M_MAX_LUN;
    next(h);
}

void msc_stop(usb_host_t *h)
{
    msc_t *d = &h->msc;
    ev_cancel(h->m, &d->ev);
    d->phase = PH_IDLE;
    if (d->copy && d->step == M_COPY) {
        usbhost_log(h, "the drive went away before %s was copied", d->copy_name);
        free(d->copy);
        d->copy = NULL;
    }
    d->found = d->mounted = false;
}

bool usbhost_drive_mounted(chip_t *m)
{
    usb_host_t *h = m->usb.host;
    return h && h->msc.mounted;
}

// Takes data, the UF2's bytes, and copies it once the drive is mounted, now if it is.
bool usbhost_drive_copy(chip_t *m, uint8_t *data, size_t len, const char *name)
{
    usb_host_t *h = m->usb.host;
    if (!h || h->msc.copy) {
        free(data);
        return false;
    }
    msc_t *d = &h->msc;
    d->copy = data;
    d->copy_len = len;
    snprintf(d->copy_name, sizeof(d->copy_name), "%s", name);
    if (d->mounted && d->step == M_MOUNTED && d->phase == PH_IDLE)
        next(h);
    return true;
}
