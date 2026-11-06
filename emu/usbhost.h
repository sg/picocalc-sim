#pragma once

#include "chip.h"
#include "picocalc-sim/sim.h"

// The host on the USB cable, shared by its core (usbhost.c: enumeration, control transfers, the
// CDC ACM serial port) and its mass storage driver (usbmsc.c).

enum { H_DETACHED, H_DEBOUNCE, H_RESET, H_RECOVERY, H_CONTROL, H_CONFIGURED, H_FAILED };
enum { XF_OK, XF_STALL, XF_ERROR };

#define MAX_DATA 4096
#define TX_SIZE 4096
#define MSC_BUF 16384

typedef struct usb_host usb_host_t;

// The device's serial port: its interfaces and bulk endpoints, and what is waiting to go out.
// dtr is the line state asked for: set while the port is open or being opened.
typedef struct {
    bool found, open, dtr;
    uint8_t comm_if, data_if, in_ep, out_ep;
    unsigned in_mps, out_mps;
    unsigned in_toggle, out_toggle, in_naks, out_naks, errors;
    emu_event_t in_ev, out_ev;
    uint8_t tx[TX_SIZE];
    unsigned tx_head, tx_len;
} serial_t;

// The device's drive: a mass storage interface speaking SCSI over the bulk-only transport, the FAT
// volume on it, and a UF2 being copied onto it.
typedef struct {
    bool found, mounted, failed;
    uint8_t iface, in_ep, out_ep;
    unsigned in_mps, out_mps, in_toggle, out_toggle;
    emu_event_t ev;
    int step, phase, tries;
    // The command in progress: its block wrapper, data and status wrapper.
    uint8_t cbw[31], *data;
    unsigned len, moved, naks, errors, info_len;
    bool in, csw_retried;
    uint8_t halt_ep;
    uint32_t tag;
    uint64_t deadline;
    void (*done)(usb_host_t *h, int status);
    uint32_t blocks, block_size, part_lba, root_lba, root_sectors, data_lba, cluster_sectors;
    char vendor[9], product[17], label[12], info[512];
    uint8_t buf[MSC_BUF];
    uint8_t *copy;
    size_t copy_len, copy_off;
    char copy_name[128];
    uint64_t copy_t0;
} msc_t;

typedef struct usb_host {
    chip_t *m;
    emu_event_t ev, sof_ev;
    int state, step, attempts;
    unsigned frame, addr, mps0;
    // Set once the device has taken its configuration, until it is reset or gone.
    bool configured, debug;
    // The control transfer in progress, and the class driver to tell when it ends.
    uint8_t setup[8];
    uint8_t data[MAX_DATA];
    unsigned want, got;
    int stage, errors;
    unsigned toggle, naks;
    uint64_t deadline;
    void (*ctl_done)(usb_host_t *h, int result);
    uint8_t dev[18];
    unsigned cfg_len;
    uint8_t cfg[MAX_DATA];
    uint16_t langid;
    char str[3][128];
    serial_t ser;
    msc_t msc;
    struct usbip *usbip;
} usb_host_t;

void usbhost_log(usb_host_t *h, const char *fmt, ...) __attribute__((format(PICOCALC_SIM_PRINTF_FORMAT, 2, 3)));
uint64_t usbhost_bus_time(unsigned n);
uint64_t usbhost_nak_delay(unsigned naks);
// A control transfer for a class driver once the device is configured; done then gets XF_OK, XF_STALL
// or XF_ERROR, with what came back in h->data. False while EP0 is busy.
bool usbhost_control(usb_host_t *h, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len,
                     void (*done)(usb_host_t *h, int result));

// The class drivers stand aside while the device is exported over USB/IP, and the device is
// enumerated afresh for them once it is not.
void usbhost_exported(usb_host_t *h);
void usbhost_unexported(usb_host_t *h);

bool usbip_open(usb_host_t *h, const char *spec);
void usbip_close(usb_host_t *h);
bool usbip_exported(usb_host_t *h);
void usbip_device_gone(usb_host_t *h);
void usbip_resume(usb_host_t *h);

void msc_init(usb_host_t *h);
// The device has a new configuration to look for a drive in, or has gone.
void msc_configured(usb_host_t *h);
void msc_stop(usb_host_t *h);
