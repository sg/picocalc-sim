#pragma once

// BSD sockets and Winsock differ in handle types, error codes and a few names; these keep net.c
// and tcp.c the same on both.

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

typedef SOCKET sock_t;
typedef int sock_len_t;
#define SOCK_NONE INVALID_SOCKET
#define SOCK_CONNRESET WSAECONNRESET
#define MSG_NOSIGNAL 0
#define SHUT_WR SD_SEND

static inline int sock_errno(void)
{
    return WSAGetLastError();
}

// Winsock reports a non-blocking connect that is under way as would-block.
static inline bool sock_in_progress(int err)
{
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
}

static inline bool sock_again(int err)
{
    return err == WSAEWOULDBLOCK || err == WSAEINTR;
}

static inline void sock_close(sock_t s)
{
    closesocket(s);
}

static inline bool sock_set_nonblock(sock_t s)
{
    u_long one = 1;
    return ioctlsocket(s, FIONBIO, &one) == 0;
}

static inline bool sock_startup(void)
{
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

static inline int sock_poll(struct pollfd *fds, size_t n, int timeout_ms)
{
    return WSAPoll(fds, (ULONG)n, timeout_ms);
}

// strerror knows nothing of Winsock's error numbers.
const char *sock_strerror(int err);
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef int sock_t;
typedef size_t sock_len_t;
#define SOCK_NONE (-1)
#define SOCK_CONNRESET ECONNRESET

static inline int sock_errno(void)
{
    return errno;
}

static inline bool sock_in_progress(int err)
{
    return err == EINPROGRESS;
}

static inline bool sock_again(int err)
{
    return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
}

static inline void sock_close(sock_t s)
{
    close(s);
}

static inline bool sock_set_nonblock(sock_t s)
{
    int fl = fcntl(s, F_GETFL, 0);
    return fl >= 0 && fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0;
}

static inline bool sock_startup(void)
{
    return true;
}

static inline int sock_poll(struct pollfd *fds, size_t n, int timeout_ms)
{
    return poll(fds, (nfds_t)n, timeout_ms);
}

static inline const char *sock_strerror(int err)
{
    return strerror(err);
}
#endif

// Winsock takes char buffers and int lengths where POSIX takes void and size_t.

static inline ssize_t sock_send(sock_t s, const void *buf, size_t len)
{
    return send(s, buf, (sock_len_t)len, MSG_NOSIGNAL);
}

static inline ssize_t sock_recv(sock_t s, void *buf, size_t len)
{
    return recv(s, buf, (sock_len_t)len, 0);
}

static inline ssize_t sock_sendto(sock_t s, const void *buf, size_t len, const struct sockaddr_in *to)
{
    return sendto(s, buf, (sock_len_t)len, 0, (const struct sockaddr *)to, sizeof(*to));
}

static inline ssize_t sock_recvfrom(sock_t s, void *buf, size_t len, struct sockaddr_in *from)
{
    socklen_t sl = sizeof(*from);
    return recvfrom(s, buf, (sock_len_t)len, 0, (struct sockaddr *)from, &sl);
}

static inline int sock_setopt(sock_t s, int level, int name, int value)
{
    return setsockopt(s, level, name, (const void *)&value, sizeof(value));
}

// The outcome of a non-blocking connect, once the socket polls writable.
static inline int sock_pending_error(sock_t s)
{
    int err = 0;
    socklen_t sl = sizeof(err);
    getsockopt(s, SOL_SOCKET, SO_ERROR, (void *)&err, &sl);
    return err;
}
