/* hx_clog example: minimal synchronous console + file logging. */
#include "hx_clog.h"

int main(void) {
    hx_clog_config_t config;
    hx_clog_config_default(&config);

    config.log_dir = "./logs";
    config.file_name = "demo.log";
    config.level = HX_CLOG_LEVEL_TRACE;

    if (hx_clog_init(&config) != HX_CLOG_OK) {
        return 1;
    }

    HX_LOG_TRACE("trace message %d", 1);
    HX_LOG_DEBUG("debug message");
    HX_LOG_INFO("program started, pid=%d", 1234);
    HX_LOG_WARN("config value missing, use default");
    HX_LOG_ERROR("open file failed: %s", "data.txt");
    HX_LOG_FATAL("fatal example (not exiting)");

    {
        hx_clog_stats_t stats;
        hx_clog_get_stats(&stats);
        HX_LOG_INFO("written_lines=%llu", stats.written_lines);
    }

    hx_clog_shutdown();
    return 0;
}
