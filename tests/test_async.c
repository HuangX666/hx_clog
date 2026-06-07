/* hx_clog test: async queue delivers every line with BLOCK policy (no loss). */
#include "hx_clog.h"

#include <stdio.h>

static unsigned long g_delivered = 0;

static int count_cb(hx_clog_level_t level, const char* data,
                    unsigned int size, void* user) {
    (void)level; (void)data; (void)size; (void)user;
    g_delivered++;
    return 0;
}

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while (0)

#define N 50000

int main(void) {
    hx_clog_config_t cfg;
    hx_clog_stats_t stats;
    int i;

    hx_clog_config_default(&cfg);
    cfg.enable_console = 0;
    cfg.enable_file = 0;
    cfg.mode = HX_CLOG_MODE_ASYNC;
    cfg.async_queue_size = 4096;
    cfg.async_batch_size = 256;
    cfg.flush_interval_ms = 100;
    cfg.overflow_policy = HX_CLOG_OVERFLOW_BLOCK;

    CHECK(hx_clog_init(&cfg) == HX_CLOG_OK);
    CHECK(hx_clog_add_callback_sink(count_cb, NULL) == HX_CLOG_OK);

    for (i = 0; i < N; ++i) {
        HX_LOG_INFO("async test %d", i);
    }

    hx_clog_shutdown(); /* drains queue */

    CHECK(hx_clog_get_stats(&stats) == HX_CLOG_OK);

    printf("delivered=%lu dropped=%llu high_watermark=%llu\n",
           g_delivered, stats.dropped_lines, stats.queue_high_watermark);

    /* With BLOCK policy nothing should be dropped and all must arrive. */
    CHECK(stats.dropped_lines == 0);
    CHECK(g_delivered == (unsigned long)N);

    printf("test_async: OK\n");
    return 0;
}
