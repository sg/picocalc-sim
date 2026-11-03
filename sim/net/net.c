#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net_internal.h"
#include "pcsim/net.h"
#include "pcsim/sim.h"

// The firmware's side of the network: a 10.0.2.0/24 Ethernet with the host at 10.0.2.2, which
// routes, answers ARP, DHCP and ping, and forwards DNS at 10.0.2.3 to the host's resolver. UDP and
// ping go out through host sockets here; TCP is terminated by lwIP in tcp.c.

#define UDP_IDLE_MS 120000
#define PING_TIMEOUT_MS 10000
#define MAX_UDP_PAYLOAD 1472

static const uint8_t gw_mac[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
static const uint8_t bcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static pcsim_net_rx_fn rx_fn;
static void *rx_ctx;
static uint8_t guest_mac[6];
static bool guest_mac_known;
uint32_t net_guest_ip = NET_GUEST;
bool net_link_up;

uint64_t net_now_ms(void)
{
    return pcsim_uptime_us() / 1000;
}

bool net_set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

void net_set_nodelay(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

uint32_t net_host_addr(uint32_t ip)
{
    return ip == NET_GW ? NET_LOOPBACK : ip;
}

uint32_t net_guest_addr(uint32_t ip)
{
    return (ip >> 24) == 127 ? NET_GW : ip;
}

// ---------------------------------------------------------------------------------------------
// Checksums and sending

uint16_t net_csum(const void *data, size_t len, uint32_t sum)
{
    const uint8_t *p = data;
    for (; len > 1; len -= 2, p += 2)
        sum += (uint32_t)(p[0] << 8 | p[1]);
    if (len)
        sum += (uint32_t)p[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

uint32_t net_csum_pseudo(uint32_t src, uint32_t dst, uint8_t proto, size_t len)
{
    return (src >> 16) + (src & 0xffff) + (dst >> 16) + (dst & 0xffff) + proto + (uint32_t)len;
}

void net_fix_checksums(uint8_t *pkt, size_t len)
{
    size_t ihl = (size_t)(pkt[0] & 15) * 4;
    if (len < ihl)
        return;
    net_wr16(pkt + 10, 0);
    net_wr16(pkt + 10, net_csum(pkt, ihl, 0));
    uint32_t src = net_rd32(pkt + 12), dst = net_rd32(pkt + 16);
    uint8_t *t = pkt + ihl;
    size_t tl = len - ihl;
    uint32_t ps = net_csum_pseudo(src, dst, pkt[9], tl);
    if (pkt[9] == 6 && tl >= 20) {
        net_wr16(t + 16, 0);
        net_wr16(t + 16, net_csum(t, tl, ps));
    } else if (pkt[9] == 17 && tl >= 8) {
        net_wr16(t + 6, 0);
        uint16_t c = net_csum(t, tl, ps);
        net_wr16(t + 6, c ? c : 0xffff);
    } else if (pkt[9] == 1 && tl >= 4) {
        net_wr16(t + 2, 0);
        net_wr16(t + 2, net_csum(t, tl, 0));
    }
}

static void send_frame(const uint8_t *dst, uint16_t type, const uint8_t *payload, size_t len)
{
    if (!rx_fn || !net_link_up || len > 1500)
        return;
    uint8_t f[1514];
    memcpy(f, dst, 6);
    memcpy(f + 6, gw_mac, 6);
    net_wr16(f + 12, type);
    memcpy(f + 14, payload, len);
    rx_fn(rx_ctx, f, len + 14);
}

void net_send_ip(const uint8_t *pkt, size_t len)
{
    send_frame(guest_mac_known ? guest_mac : bcast_mac, 0x0800, pkt, len);
}

static size_t build_udp(uint8_t *pkt, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *data,
                        size_t len)
{
    static uint16_t ip_id;
    memset(pkt, 0, 28);
    pkt[0] = 0x45;
    net_wr16(pkt + 2, (uint32_t)(28 + len));
    net_wr16(pkt + 4, ip_id++);
    pkt[8] = 64;
    pkt[9] = 17;
    net_wr32(pkt + 12, src);
    net_wr32(pkt + 16, dst);
    net_wr16(pkt + 20, sport);
    net_wr16(pkt + 22, dport);
    net_wr16(pkt + 24, (uint32_t)(8 + len));
    memcpy(pkt + 28, data, len);
    net_fix_checksums(pkt, 28 + len);
    return 28 + len;
}

void net_send_udp(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *data, size_t len)
{
    if (len > MAX_UDP_PAYLOAD)
        return;
    uint8_t pkt[1500];
    net_send_ip(pkt, build_udp(pkt, src, sport, dst, dport, data, len));
}

// ---------------------------------------------------------------------------------------------
// ARP and DHCP

static void arp_input(const uint8_t *a, size_t len)
{
    if (len < 28 || net_rd16(a) != 1 || net_rd16(a + 2) != 0x0800 || net_rd16(a + 6) != 1)
        return;
    uint32_t tpa = net_rd32(a + 24);
    if (tpa != NET_GW && tpa != NET_DNS)
        return;
    uint8_t r[28];
    memcpy(r, a, 6);
    net_wr16(r + 6, 2);
    memcpy(r + 8, gw_mac, 6);
    net_wr32(r + 14, tpa);
    memcpy(r + 18, a + 8, 10);
    send_frame(a + 8, 0x0806, r, sizeof(r));
}

static const uint8_t *dhcp_option(const uint8_t *opt, size_t len, uint8_t code, size_t *olen)
{
    size_t i = 0;
    while (i < len && opt[i] != 255) {
        if (opt[i] == 0) {
            i++;
            continue;
        }
        if (i + 1 >= len || i + 2 + opt[i + 1] > len)
            break;
        if (opt[i] == code) {
            *olen = opt[i + 1];
            return opt + i + 2;
        }
        i += 2 + opt[i + 1];
    }
    return NULL;
}

static void dhcp_input(const uint8_t *d, size_t len)
{
    if (len < 240 || d[0] != 1 || net_rd32(d + 236) != 0x63825363)
        return;
    size_t olen;
    const uint8_t *t = dhcp_option(d + 240, len - 240, 53, &olen);
    if (!t || olen != 1)
        return;
    uint8_t reply_type;
    switch (t[0]) {
    case 1: reply_type = 2; break;
    case 3: {
        const uint8_t *req = dhcp_option(d + 240, len - 240, 50, &olen);
        uint32_t want = req && olen == 4 ? net_rd32(req) : net_rd32(d + 12);
        reply_type = want == NET_GUEST ? 5 : 6;
        break;
    }
    case 8: reply_type = 5; break;
    default: return;
    }
    uint8_t r[300];
    memset(r, 0, sizeof(r));
    r[0] = 2;
    r[1] = 1;
    r[2] = 6;
    memcpy(r + 4, d + 4, 4);
    memcpy(r + 10, d + 10, 2);
    memcpy(r + 12, d + 12, 4);
    if (reply_type == 2 || (reply_type == 5 && t[0] != 8))
        net_wr32(r + 16, NET_GUEST);
    net_wr32(r + 20, NET_GW);
    memcpy(r + 28, d + 28, 16);
    net_wr32(r + 236, 0x63825363);
    uint8_t *o = r + 240;
    *o++ = 53, *o++ = 1, *o++ = reply_type;
    *o++ = 54, *o++ = 4, net_wr32(o, NET_GW), o += 4;
    if (reply_type != 6) {
        *o++ = 51, *o++ = 4, net_wr32(o, 86400), o += 4;
        *o++ = 1, *o++ = 4, net_wr32(o, NET_MASK), o += 4;
        *o++ = 3, *o++ = 4, net_wr32(o, NET_GW), o += 4;
        *o++ = 6, *o++ = 4, net_wr32(o, NET_DNS), o += 4;
        *o++ = 28, *o++ = 4, net_wr32(o, NET_BCAST), o += 4;
    }
    *o++ = 255;
    uint8_t pkt[1500];
    size_t n = build_udp(pkt, NET_GW, 67, 0xffffffffu, 68, r, sizeof(r));
    send_frame(d + 28, 0x0800, pkt, n);
    if (reply_type == 5 && t[0] == 3)
        net_guest_ip = NET_GUEST;
}

// ---------------------------------------------------------------------------------------------
// DNS, resolved on worker threads so a slow lookup never stalls the emulation

typedef struct dns_job {
    struct dns_job *next;
    unsigned gen;
    uint32_t server;
    uint16_t client_port;
    uint8_t query[512];
    size_t qend;
    char name[256];
    int rcode;
    int naddr;
    uint32_t addr[8];
} dns_job_t;

static pthread_mutex_t dns_lock = PTHREAD_MUTEX_INITIALIZER;
static dns_job_t *dns_done;
static unsigned dns_gen;
static int dns_inflight;

static void *dns_worker(void *arg)
{
    dns_job_t *j = arg;
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
    int rc = getaddrinfo(j->name, NULL, &hints, &res);
    if (rc == 0) {
        for (struct addrinfo *ai = res; ai && j->naddr < 8; ai = ai->ai_next) {
            uint32_t a = ntohl(((struct sockaddr_in *)ai->ai_addr)->sin_addr.s_addr);
            bool dup = false;
            for (int i = 0; i < j->naddr; i++)
                dup |= j->addr[i] == a;
            if (!dup)
                j->addr[j->naddr++] = a;
        }
        freeaddrinfo(res);
        j->rcode = j->naddr ? 0 : 3;
    } else {
        j->rcode = rc == EAI_NONAME
#ifdef EAI_NODATA
                           || rc == EAI_NODATA
#endif
                       ? 3
                       : 2;
    }
    pthread_mutex_lock(&dns_lock);
    j->next = dns_done;
    dns_done = j;
    pthread_mutex_unlock(&dns_lock);
    return NULL;
}

static void dns_reply(uint32_t server, uint16_t port, const uint8_t *q, size_t qend, int rcode, const uint32_t *addr,
                      int naddr)
{
    uint8_t r[512];
    if (qend > sizeof(r) - 16 * 8)
        return;
    memcpy(r, q, qend);
    net_wr16(r + 2, 0x8080u | (net_rd16(q + 2) & 0x0100u) | (unsigned)rcode);
    net_wr16(r + 4, 1);
    net_wr16(r + 6, (uint32_t)naddr);
    net_wr16(r + 8, 0);
    net_wr16(r + 10, 0);
    size_t n = qend;
    for (int i = 0; i < naddr; i++) {
        net_wr16(r + n, 0xc00c);
        net_wr16(r + n + 2, 1);
        net_wr16(r + n + 4, 1);
        net_wr32(r + n + 6, 60);
        net_wr16(r + n + 10, 4);
        net_wr32(r + n + 12, net_guest_addr(addr[i]));
        n += 16;
    }
    net_send_udp(server, 53, net_guest_ip, port, r, n);
}

static void dns_input(uint32_t server, uint16_t sport, const uint8_t *q, size_t len)
{
    if (len < 17 || (q[2] & 0x80))
        return;
    char name[256];
    size_t i = 12, nl = 0;
    while (i < len && q[i]) {
        size_t l = q[i];
        if (l > 63 || i + 1 + l >= len || nl + l + 1 >= sizeof(name)) {
            dns_reply(server, sport, q, len < 12 ? len : 12, 1, NULL, 0);
            return;
        }
        if (nl)
            name[nl++] = '.';
        memcpy(name + nl, q + i + 1, l);
        nl += l;
        i += 1 + l;
    }
    name[nl] = 0;
    if (i + 5 > len || net_rd16(q + 4) != 1)
        return;
    size_t qend = i + 5;
    uint16_t qtype = net_rd16(q + i + 1);
    if (qtype == 28) {
        dns_reply(server, sport, q, qend, 0, NULL, 0);
        return;
    }
    if (qtype != 1 || !nl) {
        dns_reply(server, sport, q, qend, 4, NULL, 0);
        return;
    }
    if (dns_inflight >= 16 || qend > sizeof(((dns_job_t *)0)->query)) {
        dns_reply(server, sport, q, qend, 2, NULL, 0);
        return;
    }
    dns_job_t *j = calloc(1, sizeof(*j));
    j->gen = dns_gen;
    j->server = server;
    j->client_port = sport;
    memcpy(j->query, q, qend);
    j->qend = qend;
    memcpy(j->name, name, nl + 1);
    pthread_t t;
    if (pthread_create(&t, NULL, dns_worker, j) != 0) {
        free(j);
        dns_reply(server, sport, q, qend, 2, NULL, 0);
        return;
    }
    pthread_detach(t);
    dns_inflight++;
}

static void dns_poll(void)
{
    pthread_mutex_lock(&dns_lock);
    dns_job_t *list = dns_done;
    dns_done = NULL;
    pthread_mutex_unlock(&dns_lock);
    while (list) {
        dns_job_t *j = list;
        list = j->next;
        dns_inflight--;
        if (j->gen == dns_gen)
            dns_reply(j->server, j->client_port, j->query, j->qend, j->rcode, j->addr, j->naddr);
        free(j);
    }
}

// ---------------------------------------------------------------------------------------------
// UDP, one host socket per firmware port so replies from anywhere find their way back

typedef struct udp_flow {
    struct udp_flow *next;
    uint16_t gport;
    int fd;
    uint64_t last;
} udp_flow_t;

static udp_flow_t *udp_flows;

static udp_flow_t *udp_flow(uint16_t gport)
{
    for (udp_flow_t *f = udp_flows; f; f = f->next)
        if (f->gport == gport)
            return f;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return NULL;
    struct sockaddr_in sa = {.sin_family = AF_INET};
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || !net_set_nonblock(fd)) {
        close(fd);
        return NULL;
    }
    udp_flow_t *f = calloc(1, sizeof(*f));
    f->gport = gport;
    f->fd = fd;
    f->next = udp_flows;
    udp_flows = f;
    return f;
}

static void udp_close_all(void)
{
    while (udp_flows) {
        udp_flow_t *f = udp_flows;
        udp_flows = f->next;
        close(f->fd);
        free(f);
    }
}

static void udp_input(const uint8_t *ip, const uint8_t *u, size_t len)
{
    if (len < 8)
        return;
    uint32_t dst = net_rd32(ip + 16);
    uint16_t sport = net_rd16(u), dport = net_rd16(u + 2);
    size_t ulen = net_rd16(u + 4);
    if (ulen < 8 || ulen > len)
        return;
    const uint8_t *data = u + 8;
    size_t n = ulen - 8;
    if (dport == 67) {
        dhcp_input(data, n);
        return;
    }
    if (dport == 53 && (dst == NET_DNS || dst == NET_GW)) {
        dns_input(dst, sport, data, n);
        return;
    }
    if (dst == 0xffffffffu || dst == NET_BCAST || (dst >> 28) == 14 || dst == NET_DNS)
        return;
    udp_flow_t *f = udp_flow(sport);
    if (!f)
        return;
    f->last = net_now_ms();
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(dport),
                             .sin_addr.s_addr = htonl(net_host_addr(dst))};
    sendto(f->fd, data, n, 0, (struct sockaddr *)&sa, sizeof(sa));
}

static void udp_service(udp_flow_t *f)
{
    uint8_t buf[2048];
    for (;;) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        ssize_t n = recvfrom(f->fd, buf, sizeof(buf), 0, (struct sockaddr *)&sa, &sl);
        if (n < 0)
            return;
        f->last = net_now_ms();
        net_send_udp(net_guest_addr(ntohl(sa.sin_addr.s_addr)), ntohs(sa.sin_port), net_guest_ip, f->gport, buf,
                     (size_t)n);
    }
}

// ---------------------------------------------------------------------------------------------
// Ping, through an unprivileged ICMP socket where the host allows one

typedef struct {
    uint32_t dst;
    uint16_t seq, id;
    uint64_t when;
} ping_t;

static int ping_fd = -1;
static bool ping_failed;
static ping_t pings[32];

static void icmp_reply(uint32_t src, const uint8_t *icmp, size_t len)
{
    uint8_t pkt[1500];
    if (len > sizeof(pkt) - 20)
        return;
    memset(pkt, 0, 20);
    pkt[0] = 0x45;
    net_wr16(pkt + 2, (uint32_t)(20 + len));
    pkt[8] = 64;
    pkt[9] = 1;
    net_wr32(pkt + 12, src);
    net_wr32(pkt + 16, net_guest_ip);
    memcpy(pkt + 20, icmp, len);
    pkt[20] = 0;
    net_fix_checksums(pkt, 20 + len);
    net_send_ip(pkt, 20 + len);
}

static void icmp_input(const uint8_t *ip, const uint8_t *icmp, size_t len)
{
    if (len < 8 || icmp[0] != 8)
        return;
    uint32_t dst = net_rd32(ip + 16);
    if (dst == NET_GW || dst == NET_DNS) {
        icmp_reply(dst, icmp, len);
        return;
    }
    if (ping_fd < 0 && !ping_failed) {
        ping_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
        if (ping_fd < 0 || !net_set_nonblock(ping_fd)) {
            ping_failed = true;
            if (ping_fd >= 0)
                close(ping_fd);
            ping_fd = -1;
            pcsim_log("net: the host does not allow unprivileged ping sockets, so pings beyond 10.0.2.2 go "
                      "unanswered (on Linux see net.ipv4.ping_group_range)");
        }
    }
    if (ping_fd < 0)
        return;
    int slot = 0;
    for (int i = 0; i < 32; i++)
        if (pings[i].when < pings[slot].when)
            slot = i;
    pings[slot] = (ping_t){dst, net_rd16(icmp + 6), net_rd16(icmp + 4), net_now_ms() | 1};
    uint8_t buf[1500];
    memcpy(buf, icmp, len);
    net_wr16(buf + 2, 0);
    net_wr16(buf + 2, net_csum(buf, len, 0));
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(dst)};
    sendto(ping_fd, buf, len, 0, (struct sockaddr *)&sa, sizeof(sa));
}

static void ping_service(void)
{
    uint8_t buf[2048];
    for (;;) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        ssize_t n = recvfrom(ping_fd, buf, sizeof(buf), 0, (struct sockaddr *)&sa, &sl);
        if (n < 0)
            return;
        uint8_t *icmp = buf;
        size_t len = (size_t)n;
        // Some hosts hand ICMP sockets the whole IP packet.
        if (len >= 28 && (buf[0] >> 4) == 4) {
            size_t ihl = (size_t)(buf[0] & 15) * 4;
            icmp += ihl;
            len -= ihl;
        }
        if (len < 8 || icmp[0] != 0)
            continue;
        uint32_t src = ntohl(sa.sin_addr.s_addr);
        uint16_t seq = net_rd16(icmp + 6);
        for (int i = 0; i < 32; i++) {
            if (pings[i].when && pings[i].dst == src && pings[i].seq == seq) {
                net_wr16(icmp + 4, pings[i].id);
                pings[i].when = 0;
                uint8_t pkt[1500];
                if (len > sizeof(pkt) - 20)
                    break;
                memset(pkt, 0, 20);
                pkt[0] = 0x45;
                net_wr16(pkt + 2, (uint32_t)(20 + len));
                pkt[8] = 64;
                pkt[9] = 1;
                net_wr32(pkt + 12, src);
                net_wr32(pkt + 16, net_guest_ip);
                memcpy(pkt + 20, icmp, len);
                net_fix_checksums(pkt, 20 + len);
                net_send_ip(pkt, 20 + len);
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Entry points

static void ipv4_input(uint8_t *ip, size_t len)
{
    if (len < 20 || (ip[0] >> 4) != 4)
        return;
    size_t ihl = (size_t)(ip[0] & 15) * 4, tot = net_rd16(ip + 2);
    if (ihl < 20 || tot < ihl || tot > len)
        return;
    if (net_rd16(ip + 6) & 0x3fff)
        return;
    uint32_t src = net_rd32(ip + 12);
    if (src && src != 0xffffffffu && (src & NET_MASK) == (NET_GUEST & NET_MASK))
        net_guest_ip = src;
    switch (ip[9]) {
    case 1: icmp_input(ip, ip + ihl, tot - ihl); break;
    case 6: tcpnat_input(ip, tot); break;
    case 17: udp_input(ip, ip + ihl, tot - ihl); break;
    default: break;
    }
}

void pcsim_net_input(const uint8_t *frame, size_t len)
{
    if (len < 14 || len > 1518)
        return;
    if (!(frame[6] & 1)) {
        memcpy(guest_mac, frame + 6, 6);
        guest_mac_known = true;
    }
    uint8_t buf[1518];
    memcpy(buf, frame, len);
    uint16_t type = net_rd16(buf + 12);
    if (type == 0x0806)
        arp_input(buf + 14, len - 14);
    else if (type == 0x0800)
        ipv4_input(buf + 14, len - 14);
}

void pcsim_net_attach(pcsim_net_rx_fn fn, void *ctx)
{
    rx_fn = fn;
    rx_ctx = ctx;
}

void pcsim_net_link(bool up)
{
    if (up == net_link_up)
        return;
    net_link_up = up;
    if (!up) {
        tcpnat_reset();
        udp_close_all();
        dns_gen++;
        memset(pings, 0, sizeof(pings));
    }
}

void pcsim_net_poll(void)
{
    dns_poll();
    pollfd_t fds[256];
    int n = 0;
    udp_flow_t *fl[64];
    int nudp = 0;
    uint64_t now = net_now_ms();
    for (udp_flow_t **pf = &udp_flows; *pf;) {
        udp_flow_t *f = *pf;
        if (now - f->last > UDP_IDLE_MS) {
            *pf = f->next;
            close(f->fd);
            free(f);
            continue;
        }
        if (nudp < 64) {
            fl[nudp++] = f;
            fds[n++] = (pollfd_t){.fd = f->fd, .events = POLLIN};
        }
        pf = &f->next;
    }
    int ping_slot = -1;
    if (ping_fd >= 0) {
        ping_slot = n;
        fds[n++] = (pollfd_t){.fd = ping_fd, .events = POLLIN};
        for (int i = 0; i < 32; i++)
            if (pings[i].when && now - pings[i].when > PING_TIMEOUT_MS)
                pings[i].when = 0;
    }
    int tcp_base = n;
    n += tcpnat_pollfds(fds + n, (int)(sizeof(fds) / sizeof(fds[0])) - n);
    if (n && poll(fds, (nfds_t)n, 0) > 0) {
        for (int i = 0; i < nudp; i++)
            if (fds[i].revents & POLLIN)
                udp_service(fl[i]);
        if (ping_slot >= 0 && (fds[ping_slot].revents & POLLIN))
            ping_service();
        tcpnat_service(fds + tcp_base, n - tcp_base);
    }
    tcpnat_timers();
}

bool pcsim_net_init(void)
{
    tcpnat_init();
    bool ok = true;
    for (int i = 0; i < pcsim_opts.forward_count; i++)
        ok &= tcpnat_forward(pcsim_opts.forwards[i]);
    return ok;
}
