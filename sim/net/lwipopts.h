#pragma once

// lwIP as the NAT's TCP endpoint only: no OS, no Ethernet layer (frames are built in net.c), no
// UDP or ICMP (handled in net.c), and memory from the C library.

#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_TIMERS 1

#define MEM_LIBC_MALLOC 1
#define MEMP_MEM_MALLOC 1
#define MEM_ALIGNMENT 8

#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ARP 0
#define LWIP_ETHERNET 0
#define LWIP_ICMP 0
#define LWIP_RAW 0
#define LWIP_UDP 0
#define LWIP_TCP 1
#define LWIP_DHCP 0
#define LWIP_AUTOIP 0
#define LWIP_ACD 0
#define LWIP_IGMP 0
#define LWIP_DNS 0
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0
#define LWIP_STATS 0
#define LWIP_NETIF_LOOPBACK 0
#define IP_REASSEMBLY 0
#define IP_FRAG 0
#define LWIP_NETIF_TX_SINGLE_PBUF 1
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS 1

#define TCP_MSS 1460
#define TCP_WND (16 * TCP_MSS)
#define TCP_SND_BUF (16 * TCP_MSS)
#define TCP_SND_QUEUELEN (4 * TCP_SND_BUF / TCP_MSS)
#define MEMP_NUM_TCP_SEG (4 * TCP_SND_QUEUELEN)
#define MEMP_NUM_TCP_PCB 256
#define MEMP_NUM_TCP_PCB_LISTEN 64
#define PBUF_POOL_SIZE 64
#define TCP_QUEUE_OOSEQ 1
#define LWIP_TCP_KEEPALIVE 0
#define TCP_LISTEN_BACKLOG 0
