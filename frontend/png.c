#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "picocalc-sim/frontend.h"

// A dependency-free PNG writer: stored (uncompressed) deflate blocks inside a valid zlib stream.

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        crc = crc_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return crc;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool chunk(FILE *f, const char *type, const uint8_t *data, size_t len)
{
    uint8_t hdr[8];
    put32(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    uint32_t crc = crc_update(0xffffffffu, hdr + 4, 4);
    crc = crc_update(crc, data, len) ^ 0xffffffffu;
    uint8_t tail[4];
    put32(tail, crc);
    return fwrite(hdr, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) && fwrite(tail, 1, 4, f) == 4;
}

bool picocalc_sim_png_write(const char *path, const uint32_t *argb, int width, int height)
{
    if (!crc_table[1])
        crc_init();

    size_t row = 1 + (size_t)width * 3;
    size_t raw_len = row * (size_t)height;
    uint8_t *raw = malloc(raw_len);
    if (!raw)
        return false;
    for (int y = 0; y < height; y++) {
        uint8_t *r = raw + row * (size_t)y;
        r[0] = 0;
        for (int x = 0; x < width; x++) {
            uint32_t px = argb[(size_t)y * (size_t)width + (size_t)x];
            r[1 + x * 3] = (uint8_t)(px >> 16);
            r[2 + x * 3] = (uint8_t)(px >> 8);
            r[3 + x * 3] = (uint8_t)px;
        }
    }

    size_t blocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + blocks * 5 + 4;
    uint8_t *z = malloc(z_len);
    if (!z) {
        free(raw);
        return false;
    }
    size_t zp = 0;
    z[zp++] = 0x78;
    z[zp++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw_len; off += 65535) {
        size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
        z[zp++] = off + n >= raw_len ? 1 : 0;
        z[zp++] = (uint8_t)n;
        z[zp++] = (uint8_t)(n >> 8);
        z[zp++] = (uint8_t)~n;
        z[zp++] = (uint8_t)(~n >> 8);
        memcpy(z + zp, raw + off, n);
        zp += n;
        for (size_t i = 0; i < n; i++) {
            a = (a + raw[off + i]) % 65521;
            b = (b + a) % 65521;
        }
    }
    put32(z + zp, b << 16 | a);
    zp += 4;

    uint8_t ihdr[13];
    put32(ihdr, (uint32_t)width);
    put32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8;
    ihdr[9] = 2;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;

    FILE *f = fopen(path, "wb");
    bool ok = f != NULL;
    if (ok) {
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        ok = fwrite(sig, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) && chunk(f, "IDAT", z, zp) &&
             chunk(f, "IEND", NULL, 0);
        ok = (fclose(f) == 0) && ok;
    }
    // The caller reports why a write failed.
    int err = errno;
    free(raw);
    free(z);
    errno = err;
    return ok;
}
