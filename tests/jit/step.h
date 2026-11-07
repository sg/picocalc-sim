// The instructions under test, shared with build.sh's check of EXPECTED on the computer building it.
#include <stdint.h>

#define EXPECTED 0xaefa06f7u

#ifdef __arm__
// High registers force the 32-bit encodings, which the JIT compiles; the low-register REVs are 16-bit.
static inline uint32_t step(uint32_t x)
{
    register uint32_t a asm("r9") = x, b asm("r12"), c asm("r10"), d asm("r11"), e asm("r8");
    asm volatile("rev.w %0, %4\n\trev16.w %1, %4\n\trevsh.w %2, %4\n\tclz %3, %4"
                 : "=r"(b), "=r"(c), "=r"(d), "=r"(e)
                 : "r"(a));
    uint32_t f, g, h;
    asm volatile("rev %0, %3\n\trev16 %1, %3\n\trevsh %2, %3" : "=l"(f), "=l"(g), "=l"(h) : "l"(x));
    return (b ^ (c << 1) ^ (d >> 3)) + e * 2654435761u + x + (f ^ g ^ (h << 5));
}
#else
static inline uint32_t step(uint32_t x)
{
    uint32_t b = __builtin_bswap32(x);
    uint32_t c = ((x >> 8) & 0x00ff00ffu) | ((x << 8) & 0xff00ff00u);
    uint32_t d = (uint32_t)(int32_t)(int16_t)__builtin_bswap16((uint16_t)x);
    uint32_t e = x ? (uint32_t)__builtin_clz(x) : 32;
    return (b ^ (c << 1) ^ (d >> 3)) + e * 2654435761u + x + (b ^ c ^ (d << 5));
}
#endif

// Enough iterations for the JIT to compile the loop, with zero and small values for CLZ.
static uint32_t run(void)
{
    uint32_t x = 0x12345678u, sum = 0;
    for (int i = 0; i < 200000; i++) {
        x = step(x) ^ (uint32_t)i;
        sum += x;
        if ((i & 0xffff) == 0)
            x >>= (i >> 16) & 31;
        if ((i & 0x3fff) == 0x2000)
            x = 0;
    }
    return sum ^ x;
}
