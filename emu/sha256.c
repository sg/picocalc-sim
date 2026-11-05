#include <string.h>

#include "chip.h"

// The RP2350's SHA-256 accelerator. Software pads the message itself, so this only digests the
// 512-bit blocks written to WDATA. Each block is digested at once rather than over 57 cycles,
// which firmware cannot tell apart except by timing, as it polls WDATA_RDY between blocks.

#define CSR_START 0x1u
#define CSR_WDATA_RDY 0x2u
#define CSR_SUM_VLD 0x4u
#define CSR_ERR_WDATA_NOT_RDY 0x10u
#define CSR_DMA_SIZE 0x300u
#define CSR_BSWAP 0x1000u

static const uint32_t k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t ror(uint32_t x, int n)
{
    return x >> n | x << (32 - n);
}

static void digest_block(uint32_t sum[8], const uint32_t block[16])
{
    uint32_t w[64], a = sum[0], b = sum[1], c = sum[2], d = sum[3], e = sum[4], f = sum[5], g = sum[6],
                    h = sum[7];
    for (int i = 0; i < 64; i++) {
        if (i < 16) {
            w[i] = block[i];
        } else {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    sum[0] += a;
    sum[1] += b;
    sum[2] += c;
    sum[3] += d;
    sum[4] += e;
    sum[5] += f;
    sum[6] += g;
    sum[7] += h;
}

static void start(chip_t *m)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(m->sha256.sum, iv, sizeof(iv));
    m->sha256.words = m->sha256.partial_bytes = 0;
    m->sha256.partial = 0;
}

void sha256_reset(chip_t *m)
{
    memset(&m->sha256, 0, sizeof(m->sha256));
    m->sha256.csr = CSR_BSWAP | 2u << 8;
    // There is no FIFO and the core is always ready, so the DMA may always send the next block.
    chip_dreq(m, DREQ_SHA256, true);
}

static void put_word(chip_t *m, uint32_t w)
{
    if (m->sha256.csr & CSR_BSWAP)
        w = __builtin_bswap32(w);
    m->sha256.block[m->sha256.words++] = w;
    if (m->sha256.words == 16) {
        digest_block(m->sha256.sum, m->sha256.block);
        m->sha256.words = 0;
    }
}

uint32_t sha256_read(chip_t *m, uint32_t off)
{
    if (off == 0x00) {
        bool valid = m->sha256.words == 0 && m->sha256.partial_bytes == 0;
        return (m->sha256.csr & (CSR_ERR_WDATA_NOT_RDY | CSR_DMA_SIZE | CSR_BSWAP)) | CSR_WDATA_RDY |
               (valid ? CSR_SUM_VLD : 0);
    }
    if (off >= 0x08 && off < 0x28)
        return m->sha256.sum[(off - 0x08) >> 2];
    return 0;
}

void sha256_write(chip_t *m, uint32_t off, uint32_t v)
{
    if (off == 0x00) {
        uint32_t keep = m->sha256.csr & CSR_ERR_WDATA_NOT_RDY & ~v;
        m->sha256.csr = (v & (CSR_DMA_SIZE | CSR_BSWAP)) | keep;
        if (v & CSR_START)
            start(m);
        return;
    }
    if (off != 0x04)
        return;
    int size = m->bus_write_size;
    if (size == 4) {
        put_word(m, v);
        return;
    }
    // Narrower writes gather in a 32-bit shift register, first byte lowest, before BSWAP applies.
    for (int i = 0; i < size; i++) {
        m->sha256.partial |= (v >> (8 * i) & 0xffu) << (8 * m->sha256.partial_bytes);
        if (++m->sha256.partial_bytes == 4) {
            put_word(m, m->sha256.partial);
            m->sha256.partial = 0;
            m->sha256.partial_bytes = 0;
        }
    }
}
