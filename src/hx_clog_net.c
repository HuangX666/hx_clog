/*
 * hx_clog - TCP/UDP network sink.
 *
 * Sends each formatted line to a remote host:port. TCP keeps a connection and
 * reconnects (rate-limited) after a failure; UDP is connectionless fire-and-
 * forget. Connection is established lazily on the first write, so a
 * temporarily-down collector does not fail init.
 *
 * Latency bounding (this sink runs under the core sink_lock like every other
 * sink, so every wait here is a wait imposed on all logging):
 *   - DNS is resolved ONCE at sink creation (before the sink ever runs under
 *     the lock); the resolved addresses are cached and reconnects never
 *     re-resolve unless the cache was invalidated by a failed attempt.
 *   - connect() is non-blocking plus a poll() timeout (HX_NET_CONNECT_MS).
 *   - steady-state TCP send() carries SO_SNDTIMEO (HX_NET_SEND_TIMEOUT_MS):
 *     a collector that accepts the connection but stops reading drops the
 *     link instead of freezing every logging thread.
 *   - the reconnect backoff window uses the monotonic clock, so an NTP wall
 *     time step backwards cannot disable the sink for the step duration.
 *
 * Sockets are created non-inheritable (FD_CLOEXEC / HANDLE_FLAG_INHERIT off)
 * and a forked child drops any inherited TCP connection via
 * hx_sink_net_after_fork (parent and child must not interleave on one
 * stream). UDP lines larger than the datagram limit are dropped without
 * tearing the (connectionless) link down.
 *
 * Built only when HX_CLOG_ENABLE_NET is defined. Lines are dropped while the
 * link is down (the async queue upstream provides the real buffering); drops
 * are reported through the error handler at most once per retry window.
 *
 * Copyright (c) 2026 HuangX
 * SPDX-License-Identifier: MIT
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
#  define HX_SOCK_ERR       WSAGetLastError()
#  define HX_SOCK_EINTR     WSAEINTR
#  define HX_SOCK_EAGAIN    WSAEWOULDBLOCK
#  define HX_SOCK_EMSGSIZE  WSAEMSGSIZE
#  define HX_POLL(fds, n, ms) WSAPoll((fds), (n), (ms))
typedef struct pollfd hx_pollfd_t;
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/poll.h>
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
#  define HX_SOCK_ERR       errno
#  define HX_SOCK_EINTR     EINTR
#  define HX_SOCK_EAGAIN    EAGAIN
#  define HX_SOCK_EMSGSIZE  EMSGSIZE
#  define HX_POLL(fds, n, ms) poll((fds), (n), (ms))
typedef struct pollfd hx_pollfd_t;
#endif

#define HX_NET_RETRY_SECS 2         /* min seconds between TCP (re)connect attempts */
#define HX_NET_CONNECT_MS 2000      /* TCP connect timeout */
#define HX_NET_SEND_TIMEOUT_MS 2000 /* TCP send timeout: a stalled collector must
                                     * not stall every logging thread holding
                                     * the sink lock */
#define HX_NET_ADDR_CACHE 4         /* resolved addresses remembered per sink */

typedef struct {
    int  proto;                 /* 0 = TCP, 1 = UDP */
    char host[256];
    char port[16];
    hx_socket_t fd;
    int  connected;
    long long last_attempt_ms;  /* monotonic clock */
    struct sockaddr_storage addr_cache[HX_NET_ADDR_CACHE];
    socklen_t addr_cache_len[HX_NET_ADDR_CACHE];
    int  addr_cache_n;
    int  addr_cache_valid;      /* resolved at create time; reconnects skip DNS */
    int  udp_oversize_reported;
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

/* Create a socket that child processes do not inherit: fork()+exec() tools
 * must not keep the collector connection (and POSIX processes with many
 * descriptors must not leak ours). */
static hx_socket_t net_socket(int family, int type, int protocol) {
    hx_socket_t fd = socket(family, type, protocol);
    if (fd == HX_INVALID_SOCK) {
        return fd;
    }
#if defined(HX_PLATFORM_WINDOWS)
    SetHandleInformation((HANDLE)fd, HANDLE_FLAG_INHERIT, 0);
#else
    int fl = fcntl(fd, F_GETFD, 0);
    if (fl >= 0) {
        fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
    }
#endif
    return fd;
}

static void net_set_send_timeout(hx_socket_t fd, int timeout_ms) {
#if defined(HX_PLATFORM_WINDOWS)
    DWORD v = (DWORD)timeout_ms;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&v, sizeof(v));
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#endif
}

/* Bounded TCP connect: non-blocking connect + poll, retrying EINTR with the
 * remaining budget. poll() has no FD_SETSIZE-style descriptor limit (the old
 * select()/FD_SET path wrote past the fixed fd_set on processes with >1024
 * descriptors). Returns 0 on success. */
static int net_connect_tcp(hx_socket_t fd, const struct sockaddr* addr,
                           int addrlen, int timeout_ms) {
    hx_pollfd_t pfd;
    long long deadline = hx_monotonic_ms() + timeout_ms;
    int rc;

    net_set_blocking(fd, 0);
    rc = connect(fd, addr, addrlen);
    if (rc == 0) {
        net_set_blocking(fd, 1);
        return 0; /* connected immediately */
    }
#if defined(HX_PLATFORM_WINDOWS)
    if (HX_SOCK_ERR != WSAEWOULDBLOCK) {
        return -1;
    }
#else
    if (errno != EINPROGRESS) {
        return -1;
    }
#endif
    for (;;) {
        int left;
        long long now = hx_monotonic_ms();
        if (now >= deadline) {
            return -1;
        }
        left = (int)(deadline - now);
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        rc = HX_POLL(&pfd, 1, left);
        if (rc > 0) {
            int err = 0;
            socklen_t elen = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &elen) != 0 ||
                err != 0) {
                return -1;
            }
            net_set_blocking(fd, 1);
            return 0;
        }
        if (rc == 0) {
            return -1; /* timed out */
        }
        if (HX_SOCK_ERR == HX_SOCK_EINTR) {
            continue; /* spurious wakeup (profiler signals etc.) — retry */
        }
        return -1;
    }
}

/* Resolve host:port into the sink's address cache. Called once at creation
 * (not under any lock) and again only after a failed attempt invalidated the
 * cache — never on the steady-state reconnect path, so a slow resolver cannot
 * stall the logging path. */
static void net_resolve_cache(net_impl* n) {
    struct addrinfo hints, *res = NULL, *rp;
    int i = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; /* IPv4 or IPv6 */
    hints.ai_socktype = (n->proto == 1) ? SOCK_DGRAM : SOCK_STREAM;
    if (getaddrinfo(n->host, n->port, &hints, &res) != 0 || !res) {
        n->addr_cache_valid = 0;
        return;
    }
    for (rp = res; rp && i < HX_NET_ADDR_CACHE; rp = rp->ai_next) {
        if (rp->ai_addrlen <= sizeof(struct sockaddr_storage)) {
            memcpy(&n->addr_cache[i], rp->ai_addr, rp->ai_addrlen);
            n->addr_cache_len[i] = (socklen_t)rp->ai_addrlen;
            i++;
        }
    }
    freeaddrinfo(res);
    n->addr_cache_n = i;
    n->addr_cache_valid = (i > 0);
}

/* Connect (TCP) / bind the destination (UDP) using the cached addresses. */
static int net_open_cached(net_impl* n) {
    int i;
    for (i = 0; i < n->addr_cache_n; ++i) {
        struct sockaddr* sa = (struct sockaddr*)&n->addr_cache[i];
        hx_socket_t fd = net_socket(sa->sa_family,
                                    (n->proto == 1) ? SOCK_DGRAM : SOCK_STREAM,
                                    0);
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
            memcpy(&n->udp_addr, sa, n->addr_cache_len[i]);
            n->udp_addrlen = (int)n->addr_cache_len[i];
            n->fd = fd;
            return 0;
        }
        if (net_connect_tcp(fd, sa, (int)n->addr_cache_len[i],
                            HX_NET_CONNECT_MS) == 0) {
            net_set_send_timeout(fd, HX_NET_SEND_TIMEOUT_MS);
            n->fd = fd;
            return 0;
        }
        hx_closesock(fd);
    }
    return -1;
}

static int net_open(net_impl* n) {
    if (!n->addr_cache_valid) {
        net_resolve_cache(n); /* rare: creation-time resolution failed */
    }
    if (!n->addr_cache_valid) {
        return -1;
    }
    if (net_open_cached(n) != 0) {
        /* every cached address failed: names can be re-pointed (DNS failover,
         * moved collector) — invalidate and re-resolve on the NEXT attempt */
        n->addr_cache_valid = 0;
        return -1;
    }
    n->connected = 1;
    return 0;
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

    if (!n->connected) {
        long long now = hx_monotonic_ms();
        if (now - n->last_attempt_ms < (long long)HX_NET_RETRY_SECS * 1000) {
            return HX_CLOG_ERR_PLATFORM; /* still in the backoff window */
        }
        n->last_attempt_ms = now;
        if (net_open(n) != 0) {
            hx_core_report_error(HX_CLOG_ERR_PLATFORM,
                                 "network sink: connect failed; dropping lines "
                                 "until the next retry");
            return HX_CLOG_ERR_PLATFORM;
        }
    }

    if (n->proto == 1) {
        for (;;) {
            int s = (int)sendto(n->fd, data, (int)size, HX_SEND_FLAGS,
                                (struct sockaddr*)&n->udp_addr, n->udp_addrlen);
            if (s >= 0) {
                return HX_CLOG_OK;
            }
            if (HX_SOCK_ERR == HX_SOCK_EINTR) {
                continue; /* retry the signal, not the whole link */
            }
            if (HX_SOCK_ERR == HX_SOCK_EMSGSIZE) {
                /* line larger than a datagram: a per-line condition, not a
                 * link failure — keep the (connectionless) link up */
                if (!n->udp_oversize_reported) {
                    n->udp_oversize_reported = 1;
                    hx_core_report_error(HX_CLOG_ERR_INVALID_ARGUMENT,
                                         "network sink: UDP line exceeds the "
                                         "datagram size limit; line dropped "
                                         "(link kept)");
                }
                return HX_CLOG_ERR_INVALID_ARGUMENT;
            }
            n->last_attempt_ms = hx_monotonic_ms();
            return HX_CLOG_ERR_PLATFORM;
        }
    }

    /* TCP: send the whole line, handling partial writes. send() carries
     * SO_SNDTIMEO, so a collector that stopped reading fails within
     * HX_NET_SEND_TIMEOUT_MS instead of blocking every logging thread
     * indefinitely; EINTR is retried, any other failure drops the link and
     * starts the backoff window. */
    {
        unsigned int off = 0;
        while (off < size) {
            int s = (int)send(n->fd, data + off, (int)(size - off),
                              HX_SEND_FLAGS);
            if (s > 0) {
                off += (unsigned int)s;
                continue;
            }
            if (s < 0 && HX_SOCK_ERR == HX_SOCK_EINTR) {
                continue; /* interrupted by a signal: resume the same write */
            }
            net_disconnect(n);
            n->last_attempt_ms = hx_monotonic_ms();
            hx_core_report_error(HX_CLOG_ERR_PLATFORM,
                                 "network sink: send failed (link stalled or "
                                 "broken); reconnecting");
            return HX_CLOG_ERR_PLATFORM;
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
    n->last_attempt_ms = 0; /* attempt on the first write */

    /* Resolve here, at creation time — this runs on the caller's thread
     * BEFORE the sink ever executes under the sink lock, so the one
     * unbounded step (the resolver) cannot stall the logging path. A failed
     * resolution keeps the cache invalid; the write path then retries
     * resolution at its rate-limited backoff cadence. */
    net_resolve_cache(n);

    memset(sink, 0, sizeof(*sink));
    sink->vtable = &k_net_vtable;
    sink->impl = n;
    sink->kind = HX_SINK_KIND_NETWORK;
    sink->is_file = 0;
    sink->id = 0;
    sink->min_level = HX_CLOG_LEVEL_TRACE;
    return sink;
}

void hx_sink_net_after_fork(hx_clog_sink_t* sink) {
    net_impl* n = sink ? (net_impl*)sink->impl : NULL;
    if (n) {
        /* the connection was inherited from the parent: one TCP stream must
         * not be written by two processes (bytes would interleave and corrupt
         * lines at the collector). Drop it; the child reconnects lazily. */
        net_disconnect(n);
        n->last_attempt_ms = 0;
    }
}

#endif /* HX_CLOG_ENABLE_NET */
