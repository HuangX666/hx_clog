/* hx_clog example: asynchronous logging. */
#include "hx_clog.h"
#include <stdio.h>

int main(void) {
    hx_clog_config_t config;
    int i;
    hx_clog_stats_t stats;

    hx_clog_config_default(&config);
    config.log_dir = "./logs";
    config.file_name = "async.log";
    config.mode = HX_CLOG_MODE_ASYNC;
    config.async_queue_size = 65536;
    config.async_batch_size = 128;
    config.flush_interval_ms = 500;

    if (hx_clog_init(&config) != HX_CLOG_OK) {
        return 1;
    }

    HX_LOG_INFO("async logging enabled");
    for (i = 0; i < 100000; ++i) {
        HX_LOG_INFO("async line %d", i);
    }

    hx_clog_flush();
    hx_clog_get_stats(&stats);
    printf("written=%llu dropped=%llu high_watermark=%llu\n",
           stats.written_lines, stats.dropped_lines, stats.queue_high_watermark);

    hx_clog_shutdown(); /* must drain the queue */
    return 0;
}
