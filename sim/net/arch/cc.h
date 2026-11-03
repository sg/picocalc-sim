#pragma once

#include <stdio.h>
#include <stdlib.h>

#define LWIP_PLATFORM_DIAG(x) \
    do {                      \
        printf x;             \
    } while (0)
// An internal inconsistency in the NAT should be reported, not take the emulator down with it.
#define LWIP_PLATFORM_ASSERT(x) \
    do {                        \
        fprintf(stderr, "[pcsim] lwip: %s (%s:%d)\n", x, __FILE__, __LINE__); \
    } while (0)
#define LWIP_RAND() ((u32_t)rand())
