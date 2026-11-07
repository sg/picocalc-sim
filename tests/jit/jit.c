// A check of instructions the JIT compiles, for picocalc-sim's CI: the same loop runs several times,
// so later passes run compiled code, and each must give the value the computer building it got.
#include <stdio.h>

#include "pico/stdlib.h"
#include "step.h"

int main(void)
{
    stdio_init_all();
    int failed = 0;
    for (int pass = 0; pass < 3; pass++) {
        uint32_t got = run();
        if (got != EXPECTED) {
            printf("JIT FAIL pass %d: %08lx, expected %08lx\n", pass, (unsigned long)got, (unsigned long)EXPECTED);
            failed = 1;
        }
    }
    if (!failed)
        printf("JIT PASS: REV, REV16, REVSH and CLZ\n");
    for (;;)
        tight_loop_contents();
}
