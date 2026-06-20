/*
 * hx_clog - TCP/UDP network sink.
 *
 * Sends each formatted line to a remote host:port. TCP keeps a connection and
 * reconnects (rate-limited) after a failure; UDP is connectionless fire-and-
 * forget. Connection is established lazily on the first write, so adding the
 * sink never blocks and a temporarily-down collector does not fail init.
 *
 * Built only when HX_CLOG_ENABLE_NET is defined. Lines are dropped while the
 * link is down (the async queue upstream provides the real buffering); drops
 * are reported through the error handler at most once per retry window.
 */
#include "hx_clog_internal.h"

#if defined(HX_CLOG_ENABLE_NET)

#if defined(HX_PLATFORM_WINDOWS)
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET hx_socket_t;
#  define HX_INVALID_SOCK   INVALID_SOCKET
#  define hx_closesock(s)   closesocket(s)
#  define HX_SEND_FLAGS     0
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
typedef int hx_socket_t;
#  define HX_INVALID_SOCK   (-1)
#  define hx_closesock(s)   close(s)
#  if defined(MSG_NOSIGNAL)
#    define HX_SEND_FLAGS   MSG_NOSIGNAL   /* don't raise SIGPIPE on dead peer */
#  else
#    define HX_SEND_FLAGS   0              /* macOS: handled via SO_NOSIGPIPE */
#  endif
#endif

#define HX_NET_RETRY_SECS 2   /* min seconds between TCP (re)connect attempts */
#define HX_NET_CONNECT_MS 2000 /* TCP connect timeout */

typedef struct {
    int  proto;                 /* 0 = TCP, 1 = UDP */
    char host[256];
    char port[16];
    hx_socket_t fd;
    int  connected;
    long long last_attempt_sec;
    struct sockaddr_storage udp_addr; /* cached UDP destination */
    int  udp_addrlen;
} net_impl;

static void net_set_blocking(hx_socket_t fd, int blocking) {
#if defined(HX_PLATFORM_WINDOWS)
    u_long mode = blocking ? 0 : 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return;
    if (blocking) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    else          fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

/* Bounded TCP connect: non-blocking connect + select() so a firewalled host
 * cannot stall the (worker) thread for the full OS timeout. */
static int net_connect_tcp(hx_socket_t fd, const struct sockaddr* addr,
                           int addrlen, int timeout_ms) {
    fd_set wset;
    struct timeval tv;
    int rc;

    net_set_blocking(fd, 0);
    rc = connect(fd, addr, addrlen);
    if (rc == 0) {
        net_set_blocking(fd, 1);
        return 0; /* connected immediately */
    }
#if defined(HX_PLATFORM_WINDOWS)
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
        return -1;
    }
#else
    if (errno != EINPROGRESS) {
        return -1;
    }
#endif
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    rc = select((int)fd + 1, NULL, &wset, NULL, &tv);
    if (rc <= 0) {
        return -1; /* timed out or error */
    }
    {
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &elen) != 0 ||
            err != 0) {
            return -1;
        }
    }
    net_set_blocking(fd, 1);
    return 0;
}

/* Resolve host:port and connect (TCP) / cache the destination (UDP). */
static int net_open(net_impl* n) {
    struct addrinfo hints, *res = NULL, *rp;
    int got = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; /* IPv4 or IPv6 */
    hints.ai_socktype = (n->proto == 1) ? SOCK_DGRAM : SOCK_STREAM;
    if (getaddrinfo(n->host, n->port, &hints, &res) != 0 || !res) {
        return -1;
    }
    for (rp = res; rp; rp = rp->ai_next) {
        hx_socket_t fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == HX_INVALID_SOCK) {
            continue;
        }
#if defined(SO_NOSIGPIPE)
        {
            int on = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (char*)&on, sizeof(on));
        }
#endif
        if (n->proto == 1) {
            /* UDP: no connect, just remember where to send */
            memcpy(&n->udp_addr, rp->ai_addr, rp->ai_addrlen);
            n->udp_addrlen = (int)rp->ai_addrlen;
            n->fd = fd;
            got = 0;
            break;
        }
        if (net_connect_tcp(fd, rp->ai_addr, (int)rp->ai_addrlen,
                            HX_NET_CONNECT_MS) == 0) {
            n->fd = fd;
            got = 0;
            break;
        }
        hx_closesock(fd);
    }
    freeaddrinfo(res);
    if (got == 0) {
        n->connected = 1;
    }
    return got;
}

static void net_disconnect(net_impl* n) {
    if (n->fd != HX_INVALID_SOCK) {
        hx_closesock(n->fd);
        n->fd = HX_INVALID_SOCK;
    }
    n->connected = 0;
}

static int net_write(hx_clog_sink_t* sink, const char* data, unsigned int size) {
    net_impl* n = (net_impl*)sink->impl;
    long long now;

    if (!n->connected) {
        now = (long long)time(NULL);
        if (now - n->last_attempt_sec < HX_NET_RETRY_SECS) {
            return HX_CLOG_ERR_PLATFORM; /* still in the backoff window */
        }
        n->last_attempt_sec = now;
        if (net_open(n) != 0) {
            hx_core_report_error(HX_CLOG_ERR_PLATFORM,
                                 "network sink: connect failed; dropping lines "
                                 "until the next retry");
            return HX_CLOG_ERR_PLATFORM;
        }
    }

    if (n->proto == 1) {
        int s = (int)sendto(n->fd, data, (int)size, HX_SEND_FLAGS,
                            (struct sockaddr*)&n->udp_addr, n->udp_addrlen);
        if (s < 0) {
            net_disconnect(n);
            n->last_attempt_sec = (long long)time(NULL);
            return HX_CLOG_ERR_PLATFORM;
        }
        return HX_CLOG_OK;
    }

    /* TCP: send the whole line, handling partial writes */
    {
        unsigned int off = 0;
        while (off < size) {
            int s = (int)send(n->fd, data + off, (int)(size - off),
                              HX_SEND_FLAGS);
            if (s <= 0) {
                net_disconnect(n);
                n->last_attempt_sec = (long long)time(NULL);
                hx_core_report_error(HX_CLOG_ERR_PLATFORM,
                                     "network sink: send failed; reconnecting");
                return HX_CLOG_ERR_PLATFORM;
            }
            off += (unsigned int)s;
        }
    }
    return HX_CLOG_OK;
}

static int net_flush(hx_clog_sink_t* sink) {
    (void)sink; /* sockets are not application-buffered */
    return HX_CLOG_OK;
}

static void net_close(hx_clog_sink_t* sink) {
    net_impl* n;
    if (!sink) {
        return;
    }
    n = (net_impl*)sink->impl;
    if (n) {
        net_disconnect(n);
        hx_clog__free(n);
    }
#if defined(HX_PLATFORM_WINDOWS)
    WSACleanup(); /* balance the WSAStartup from create (refcounted) */
#endif
    hx_clog__free(sink);
}

static const hx_clog_sink_vtable_t k_net_vtable = {
    net_write, net_flush, net_close
};

hx_clog_sink_t* hx_sink_network_create(int proto, const char* host,
                                       unsigned short port) {
    hx_clog_sink_t* sink;
    net_impl* n;

    if (!host || !host[0] || port == 0) {
        return NULL;
    }
#if defined(HX_PLATFORM_WINDOWS)
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return NULL;
        }
    }
#endif
    sink = (hx_clog_sink_t*)hx_clog__malloc(sizeof(*sink));
    if (!sink) {
#if defined(HX_PLATFORM_WINDOWS)
        WSACleanup();
#endif
        return NULL;
    }
    n = (net_impl*)hx_clog__malloc(sizeof(*n));
    if (!n) {
        hx_clog__free(sink);
#if defined(HX_PLATFORM_WINDOWS)
        WSACleanup();
#endif
        return NULL;
    }
    memset(n, 0, sizeof(*n));
    n->proto = (proto == 1) ? 1 : 0;
    strncpy(n->host, host, sizeof(n->host) - 1);
    snprintf(n->port, sizeof(n->port), "%u", (unsigned)port);
    n->fd = HX_INVALID_SOCK;
    n->connected = 0;
    n->last_attempt_sec = 0; /* attempt on the first write */

    memset(sink, 0, sizeof(*sink));
    sink->vtable = &k_net_vtable;
    sink->impl = n;
    sink->kind = HX_SINK_KIND_NETWORK;
    sink->is_file = 0;
    sink->id = 0;
    sink->min_level = HX_CLOG_LEVEL_TRACE;
    return sink;
}

#endif /* HX_CLOG_ENABLE_NET */
