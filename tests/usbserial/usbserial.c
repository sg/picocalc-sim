// A self-check of the USB serial port for picocalc-sim's CI. Once a terminal opens the port it
// reports the length and hash of the first line the terminal sends, then sends 100 numbered lines,
// enough to span many packets.
#include <stdio.h>

#include "pico/stdlib.h"

int main(void)
{
    stdio_init_all();
    while (!stdio_usb_connected())
        sleep_ms(1);
    printf("USBSER READY\n");
    unsigned len = 0;
    uint32_t hash = 0;
    for (;;) {
        int c = getchar_timeout_us(5000000);
        if (c == PICO_ERROR_TIMEOUT) {
            printf("USBSER TIMEOUT after %u bytes\n", len);
            break;
        }
        if (c == '\r')
            break;
        hash = hash * 31 + (uint32_t)c;
        len++;
    }
    printf("USBSER GOT %u %08lx\n", len, (unsigned long)hash);
    for (int i = 0; i < 100; i++) {
        char pattern[49];
        for (int j = 0; j < 48; j++)
            pattern[j] = (char)('A' + (i + j) % 26);
        pattern[48] = 0;
        printf("USBSER LINE %03d %s\n", i, pattern);
    }
    printf("USBSER DONE\n");
    for (;;)
        sleep_ms(1000);
}
