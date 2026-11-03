#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sock.h"

// Shared by the NAT's protocol handlers. Addresses are host-order uint32_t throughout.

#define NET_GW 0x0a000202u
#define NET_DNS 0x0a000203u
#define NET_GUEST 0x0a00020fu
#define NET_MASK 0xffffff00u
#define NET_BCAST 0x0a0002ffu
#define NET_LOOPBACK 0x7f000001u

extern uint32_t net_guest_ip;
extern bool net_link_up;

uint64_t net_now_ms(void);
uint16_t net_csum(const void *data, size_t len, uint32_t sum);
uint32_t net_csum_pseudo(uint32_t src, uint32_t dst, uint8_t proto, size_t len);
// Sends an IPv4 packet with valid header and transport checksums to the firmware.
void net_send_ip(const uint8_t *pkt, size_t len);
void net_send_udp(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *data, size_t len);
void net_fix_checksums(uint8_t *pkt, size_t len);
// Where a destination the firmware names lives on the host: the gateway is the host itself.
uint32_t net_host_addr(uint32_t ip);
// How the firmware should see a host address: loopback becomes the gateway.
uint32_t net_guest_addr(uint32_t ip);
void net_set_nodelay(sock_t s);

static inline uint16_t net_rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static inline uint32_t net_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline void net_wr16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void net_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

typedef struct pollfd pollfd_t;

void tcpnat_init(void);
void tcpnat_input(uint8_t *pkt, size_t len);
// Adds the sockets it wants polled; tcpnat_service then handles their events.
int tcpnat_pollfds(pollfd_t *fds, int max);
void tcpnat_service(const pollfd_t *fds, int n);
void tcpnat_timers(void);
void tcpnat_reset(void);
bool tcpnat_forward(const char *spec);
