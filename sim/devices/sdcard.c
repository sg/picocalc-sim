#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "pcsim/port.h"

#include "pcsim/bus.h"
#include "pcsim/devices.h"
#include "pcsim/sim.h"

// An SDHC card in SPI mode, backed by a disk image. Commands arrive as six-byte frames; every
// response, data token and busy period goes out one byte per clocked byte, so the firmware's
// polling loops see the same sequence a real card produces.

#define BLOCK 512
#define OUT_MAX (BLOCK + 16)
#define WRITE_BUSY_BYTES 16

enum card_state { CARD_IDLE, CARD_READY };
enum xfer_mode { MODE_COMMAND, MODE_MULTI_READ, MODE_WRITE_TOKEN, MODE_WRITE_DATA };

struct sdcard {
    pthread_mutex_t lock;
    int fd;
    uint32_t blocks;
    int detect_pin;

    enum card_state state;
    bool app_cmd;
    int acmd41_tries;

    uint8_t cmd[6];
    int cmd_len;

    uint8_t out[OUT_MAX];
    int out_len, out_pos;
    int busy_bytes;

    enum xfer_mode mode;
    bool multi_write;
    uint32_t next_block;
    uint8_t wbuf[BLOCK + 2];
    int wpos;
};

static uint8_t crc7(const uint8_t *p, int n)
{
    uint8_t crc = 0;
    for (int i = 0; i < n; i++) {
        uint8_t b = p[i];
        for (int bit = 0; bit < 8; bit++) {
            crc <<= 1;
            if ((b ^ crc) & 0x80)
                crc ^= 0x09;
            b <<= 1;
        }
    }
    return crc & 0x7f;
}

static uint16_t crc16(const uint8_t *p, int n)
{
    uint16_t crc = 0;
    for (int i = 0; i < n; i++) {
        crc ^= (uint16_t)(p[i] << 8);
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 0x8000) ? (uint16_t)(crc << 1 ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static void out_reset(sdcard_t *sd)
{
    sd->out_len = sd->out_pos = 0;
}

static void out_byte(sdcard_t *sd, uint8_t b)
{
    if (sd->out_len < OUT_MAX)
        sd->out[sd->out_len++] = b;
}

static uint8_t r1(const sdcard_t *sd, uint8_t flags)
{
    return (uint8_t)((sd->state == CARD_IDLE ? 0x01 : 0x00) | flags);
}

static void queue_data_block(sdcard_t *sd, const uint8_t *data, int len)
{
    out_byte(sd, 0xff);
    out_byte(sd, 0xfe);
    for (int i = 0; i < len; i++)
        out_byte(sd, data[i]);
    uint16_t crc = crc16(data, len);
    out_byte(sd, (uint8_t)(crc >> 8));
    out_byte(sd, (uint8_t)crc);
}

static bool read_block(sdcard_t *sd, uint32_t block, uint8_t *buf)
{
    if (block >= sd->blocks)
        return false;
    ssize_t n = pread(sd->fd, buf, BLOCK, (int64_t)block * BLOCK);
    if (n < 0)
        return false;
    if (n < BLOCK)
        memset(buf + n, 0, BLOCK - (size_t)n);
    return true;
}

static void make_csd(const sdcard_t *sd, uint8_t csd[16])
{
    uint32_t c_size = sd->blocks >= 1024 ? sd->blocks / 1024 - 1 : 0;
    static const uint8_t base[16] = {0x40, 0x0e, 0x00, 0x32, 0x5b, 0x59, 0x00, 0, 0, 0, 0x7f, 0x80, 0x0a, 0x40, 0x00, 0};
    memcpy(csd, base, 16);
    csd[7] = (uint8_t)((c_size >> 16) & 0x3f);
    csd[8] = (uint8_t)(c_size >> 8);
    csd[9] = (uint8_t)c_size;
    csd[15] = (uint8_t)(crc7(csd, 15) << 1 | 1);
}

static void make_cid(uint8_t cid[16])
{
    static const uint8_t base[16] = {0x50, 'P', 'C', 'P', 'C', 'S', 'I', 'M', 0x10, 0x12, 0x34, 0x56, 0x78, 0x01, 0x9a, 0};
    memcpy(cid, base, 16);
    cid[15] = (uint8_t)(crc7(cid, 15) << 1 | 1);
}

static void execute(sdcard_t *sd)
{
    uint8_t index = sd->cmd[0] & 0x3f;
    uint32_t arg = (uint32_t)sd->cmd[1] << 24 | (uint32_t)sd->cmd[2] << 16 | (uint32_t)sd->cmd[3] << 8 | sd->cmd[4];
    bool app = sd->app_cmd;
    sd->app_cmd = false;
    out_reset(sd);
    // Ncr: one idle byte before the response.
    out_byte(sd, 0xff);

    if (app) {
        switch (index) {
        case 41:
            if (++sd->acmd41_tries >= 2)
                sd->state = CARD_READY;
            out_byte(sd, r1(sd, 0));
            return;
        case 13:
            out_byte(sd, r1(sd, 0));
            out_byte(sd, 0x00);
            return;
        case 23:
        case 42:
        case 51:
            out_byte(sd, r1(sd, 0));
            return;
        default:
            break;
        }
    }

    switch (index) {
    case 0:
        sd->state = CARD_IDLE;
        sd->acmd41_tries = 0;
        sd->mode = MODE_COMMAND;
        out_byte(sd, 0x01);
        break;
    case 1:
        if (++sd->acmd41_tries >= 2)
            sd->state = CARD_READY;
        out_byte(sd, r1(sd, 0));
        break;
    case 8:
        out_byte(sd, r1(sd, 0));
        out_byte(sd, 0x00);
        out_byte(sd, 0x00);
        out_byte(sd, (uint8_t)((arg >> 8) & 0x0f));
        out_byte(sd, (uint8_t)arg);
        break;
    case 9: {
        uint8_t csd[16];
        make_csd(sd, csd);
        out_byte(sd, r1(sd, 0));
        queue_data_block(sd, csd, 16);
        break;
    }
    case 10: {
        uint8_t cid[16];
        make_cid(cid);
        out_byte(sd, r1(sd, 0));
        queue_data_block(sd, cid, 16);
        break;
    }
    case 12:
        sd->mode = MODE_COMMAND;
        out_byte(sd, 0xff);
        out_byte(sd, r1(sd, 0));
        sd->busy_bytes = 2;
        break;
    case 13:
        out_byte(sd, r1(sd, 0));
        out_byte(sd, 0x00);
        break;
    case 16:
    case 59:
        out_byte(sd, r1(sd, 0));
        break;
    case 17:
    case 18: {
        uint8_t buf[BLOCK];
        if (sd->state != CARD_READY || !read_block(sd, arg, buf)) {
            out_byte(sd, r1(sd, 0x40));
            break;
        }
        out_byte(sd, r1(sd, 0));
        queue_data_block(sd, buf, BLOCK);
        if (index == 18) {
            sd->mode = MODE_MULTI_READ;
            sd->next_block = arg + 1;
        }
        break;
    }
    case 24:
    case 25:
        if (sd->state != CARD_READY || arg >= sd->blocks) {
            out_byte(sd, r1(sd, 0x40));
            break;
        }
        out_byte(sd, r1(sd, 0));
        sd->mode = MODE_WRITE_TOKEN;
        sd->multi_write = index == 25;
        sd->next_block = arg;
        break;
    case 55:
        sd->app_cmd = true;
        out_byte(sd, r1(sd, 0));
        break;
    case 58:
        out_byte(sd, r1(sd, 0));
        out_byte(sd, sd->state == CARD_READY ? 0xc0 : 0x40);
        out_byte(sd, 0xff);
        out_byte(sd, 0x80);
        out_byte(sd, 0x00);
        break;
    default:
        out_byte(sd, r1(sd, 0x04));
        break;
    }
}

static void write_data_byte(sdcard_t *sd, uint8_t mosi)
{
    sd->wbuf[sd->wpos++] = mosi;
    if (sd->wpos < BLOCK + 2)
        return;
    bool ok = sd->next_block < sd->blocks &&
              pwrite(sd->fd, sd->wbuf, BLOCK, (int64_t)sd->next_block * BLOCK) == BLOCK;
    sd->next_block++;
    out_reset(sd);
    // Data response token: 010 accepted, 110 write error.
    out_byte(sd, ok ? 0xe5 : 0xed);
    sd->busy_bytes = WRITE_BUSY_BYTES;
    sd->mode = sd->multi_write ? MODE_WRITE_TOKEN : MODE_COMMAND;
}

static uint8_t sd_xfer(void *ctx, uint8_t mosi)
{
    sdcard_t *sd = ctx;
    pthread_mutex_lock(&sd->lock);
    if (sd->fd < 0) {
        pthread_mutex_unlock(&sd->lock);
        return 0xff;
    }

    uint8_t miso = 0xff;
    if (sd->out_pos < sd->out_len) {
        miso = sd->out[sd->out_pos++];
    } else if (sd->busy_bytes > 0) {
        miso = 0x00;
        sd->busy_bytes--;
    } else if (sd->mode == MODE_MULTI_READ) {
        uint8_t buf[BLOCK];
        out_reset(sd);
        if (read_block(sd, sd->next_block++, buf))
            queue_data_block(sd, buf, BLOCK);
        else
            out_byte(sd, 0x08);
        miso = sd->out[sd->out_pos++];
    }

    switch (sd->mode) {
    case MODE_WRITE_TOKEN:
        if (mosi == 0xfe || (sd->multi_write && mosi == 0xfc)) {
            sd->mode = MODE_WRITE_DATA;
            sd->wpos = 0;
        } else if (sd->multi_write && mosi == 0xfd) {
            sd->mode = MODE_COMMAND;
            sd->busy_bytes = WRITE_BUSY_BYTES;
        }
        break;
    case MODE_WRITE_DATA:
        write_data_byte(sd, mosi);
        break;
    default:
        if (sd->cmd_len > 0 || (mosi & 0xc0) == 0x40) {
            sd->cmd[sd->cmd_len++] = mosi;
            if (sd->cmd_len == 6) {
                sd->cmd_len = 0;
                execute(sd);
            }
        }
        break;
    }
    pthread_mutex_unlock(&sd->lock);
    return miso;
}

static void sd_select(void *ctx, bool selected)
{
    sdcard_t *sd = ctx;
    if (selected)
        return;
    pthread_mutex_lock(&sd->lock);
    sd->cmd_len = 0;
    out_reset(sd);
    if (sd->mode == MODE_MULTI_READ)
        sd->mode = MODE_COMMAND;
    pthread_mutex_unlock(&sd->lock);
}

sdcard_t *sdcard_create(pcsim_spi_pins_t pins, int detect_pin)
{
    sdcard_t *sd = calloc(1, sizeof(*sd));
    pthread_mutex_init(&sd->lock, NULL);
    sd->fd = -1;
    sd->detect_pin = detect_pin;
    pcsim_spi_device_t *dev = calloc(1, sizeof(*dev));
    dev->name = "SD card";
    dev->sck_pin = pins.sck;
    dev->mosi_pin = pins.mosi;
    dev->miso_pin = pins.miso;
    dev->cs_pin = pins.cs;
    dev->select = sd_select;
    dev->xfer = sd_xfer;
    dev->ctx = sd;
    pcsim_spi_attach(dev);
    return sd;
}

bool sdcard_insert(sdcard_t *sd, const char *image_path)
{
    int fd = open(image_path, O_RDWR | O_BINARY);
    if (fd < 0) {
        pcsim_log("cannot open card image %s: %s", image_path, strerror(errno));
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 64 * BLOCK) {
        pcsim_log("card image %s is too small", image_path);
        close(fd);
        return false;
    }
    pthread_mutex_lock(&sd->lock);
    if (sd->fd >= 0)
        close(sd->fd);
    sd->fd = fd;
    sd->blocks = (uint32_t)(st.st_size / BLOCK);
    sd->state = CARD_IDLE;
    sd->mode = MODE_COMMAND;
    sd->cmd_len = 0;
    out_reset(sd);
    pthread_mutex_unlock(&sd->lock);
    if (sd->detect_pin >= 0)
        pcsim_gpio_drive_input((unsigned)sd->detect_pin, 0);
    pcsim_log("SD card: %s (%u MB)", image_path, (unsigned)(sd->blocks / 2048));
    return true;
}

void sdcard_eject(sdcard_t *sd)
{
    pthread_mutex_lock(&sd->lock);
    if (sd->fd >= 0) {
        fsync(sd->fd);
        close(sd->fd);
    }
    sd->fd = -1;
    pthread_mutex_unlock(&sd->lock);
    if (sd->detect_pin >= 0)
        pcsim_gpio_drive_input((unsigned)sd->detect_pin, -1);
}

void sdcard_flush(sdcard_t *sd)
{
    pthread_mutex_lock(&sd->lock);
    if (sd->fd >= 0)
        fsync(sd->fd);
    pthread_mutex_unlock(&sd->lock);
}

bool sdcard_present(sdcard_t *sd)
{
    pthread_mutex_lock(&sd->lock);
    bool present = sd->fd >= 0;
    pthread_mutex_unlock(&sd->lock);
    return present;
}
