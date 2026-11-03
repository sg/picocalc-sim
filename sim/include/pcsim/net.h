#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The user-mode network behind the emulated WiFi: a NAT router on 10.0.2.0/24 that answers DHCP
// and DNS and carries the firmware's TCP, UDP and ping traffic over the host's own sockets. All
// calls come from the emulation thread.

typedef void (*pcsim_net_rx_fn)(void *ctx, const uint8_t *frame, size_t len);

// Opens the port forwards in pcsim_opts; false if one cannot be set up.
bool pcsim_net_init(void);
void pcsim_net_attach(pcsim_net_rx_fn fn, void *ctx);
// Association with the virtual access point; dropping it closes every connection.
void pcsim_net_link(bool up);
// An Ethernet frame the firmware sent.
void pcsim_net_input(const uint8_t *frame, size_t len);
// Services host sockets and protocol timers; call every millisecond or so of emulated time.
void pcsim_net_poll(void);

#ifdef __cplusplus
}
#endif
