/*
 * hx_clog test: large log lines.
 *
 * Verifies that a single line well beyond the old limits is delivered intact
 * in BOTH sync and async modes, and that the configured cap is enforced.
 *
 * With the default 512 KB cap:
 *   - a 200 KB message is delivered in full.
 *   - a 700 KB message is truncated to <= 512 KB.
 * When built with HX_CLOG_UNLIMITED_LINE, the 700 KB message is delivered
 * in full too.
 */
#include "hx_clog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int g_last_size;
static char         g_first, g_last_char;

static int capture_cb(hx_clog_level_t level, const char* data,
                      unsigned int size, void* user) {
    (void)level; (void)user;
    g_last_size = size;
    g_first = size ? data[0] : 0;
    g_last_char = size ? data[size - 1] : 0;
    return 0;
}

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while (0)

#define K (1024u)
#define CAP_DEFAULT (512u * K)

/* Log a message of `n` 'A' bytes using pattern "%v" (line == message). */
static void log_big(unsigned int n) {
    char* buf = (char*)malloc(n + 1);
    if (!buf) { return; }
    memset(buf, 'A', n);
    buf[n] = '\0';
    HX_LOG_INFO("%s", buf);
    free(buf);
}

static int run_mode(hx_clog_mode_t mode, const char* name) {
    hx_clog_config_t cfg;
    hx_clog_config_default(&cfg);
    cfg.enable_console = 0;
    cfg.enable_file = 0;
    cfg.mode = mode;
    cfg.pattern = "%v";            /* line content == raw message */
    cfg.async_queue_size = 64;
    cfg.async_batch_size = 16;

    CHECK(hx_clog_init(&cfg) == HX_CLOG_OK);
    CHECK(hx_clog_add_callback_sink(capture_cb, NULL) == HX_CLOG_OK);

    /* 200 KB: under the default cap -> delivered in full */
    g_last_size = 0;
    log_big(200u * K);
    hx_clog_flush();
    printf("[%s] 200K delivered=%u\n", name, g_last_size);
    CHECK(g_last_size == 200u * K);
    CHECK(g_first == 'A' && g_last_char == 'A');

    /* 700 KB: beyond the default cap */
    g_last_size = 0;
    log_big(700u * K);
    hx_clog_flush();
    printf("[%s] 700K delivered=%u\n", name, g_last_size);
#if defined(HX_CLOG_UNLIMITED_LINE)
    CHECK(g_last_size == 700u * K);   /* no cap */
#else
    CHECK(g_last_size <= CAP_DEFAULT);          /* capped */
    CHECK(g_last_size >= CAP_DEFAULT - 16u);    /* but close to the cap */
#endif

    hx_clog_shutdown();
    return 0;
}

int main(void) {
    if (run_mode(HX_CLOG_MODE_SYNC, "sync") != 0) {
        return 1;
    }
    if (run_mode(HX_CLOG_MODE_ASYNC, "async") != 0) {
        return 1;
    }
    printf("test_largeline: OK\n");
    return 0;
}
