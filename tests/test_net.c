/* hx_clog test: UDP network sink end-to-end over loopback.
 *
 * Binds a UDP receiver on an ephemeral 127.0.0.1 port, points a UDP network
 * sink at it, logs a line, and verifies the bytes arrive. Only built when
 * HX_CLOG_ENABLE_NET is on. */
#include "hx_clog.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET sock_t;
#  define CLOSESOCK closesocket
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
   typedef int sock_t;
#  define CLOSESOCK close
#  define INVALID_SOCKET (-1)
#endif

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); return 1; } \
} while (0)

int main(void) {
    hx_clog_config_t cfg;
    sock_t rx;
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    unsigned short port;
    char buf[256];
    int n, i, got = 0;

#if defined(_WIN32)
    WSADATA wsa;
    CHECK(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif

    rx = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(rx != INVALID_SOCKET);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* ephemeral */
    CHECK(bind(rx, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    CHECK(getsockname(rx, (struct sockaddr*)&addr, &alen) == 0);
    port = ntohs(addr.sin_port);

    /* receive timeout so a lost datagram doesn't hang the test */
    {
#if defined(_WIN32)
        DWORD tmo = 2000;
        setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, (char*)&tmo, sizeof(tmo));
#else
        struct timeval tv; tv.tv_sec = 2; tv.tv_usec = 0;
        setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
#endif
    }

    hx_clog_config_default(&cfg);
    cfg.enable_console = 0;
    cfg.enable_file = 0;
    cfg.pattern = "%v";   /* send just the message body */
    CHECK(hx_clog_init(&cfg) == HX_CLOG_OK);
    CHECK(hx_clog_add_network_sink(HX_CLOG_NET_UDP, "127.0.0.1", port, NULL)
              == HX_CLOG_OK);

    /* UDP can drop; send a few and accept the first that arrives */
    for (i = 0; i < 5 && !got; ++i) {
        HX_LOG_INFO("hello-net-42");
        hx_clog_flush();
        n = (int)recvfrom(rx, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if (n > 0) {
            buf[n] = '\0';
            if (strstr(buf, "hello-net-42")) {
                got = 1;
            }
        }
    }

    hx_clog_shutdown();
    CLOSESOCK(rx);
#if defined(_WIN32)
    WSACleanup();
#endif

    CHECK(got == 1);
    printf("ALL PASS\n");
    return 0;
}
