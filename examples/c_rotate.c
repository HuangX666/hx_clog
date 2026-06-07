/* hx_clog example: size-based log rotation. */
#include "hx_clog.h"
#include <stdio.h>

int main(void) {
    hx_clog_config_t config;
    int i;
    hx_clog_stats_t stats;

    hx_clog_config_default(&config);
    config.log_dir = "./logs_rotate";
    config.file_name = "rotate.log";
    config.enable_console = 0; /* keep the console quiet for this demo */
    config.rotate_policy = HX_CLOG_ROTATE_BY_SIZE;
    config.max_file_size = 64ULL * 1024ULL; /* small so we rotate quickly */
    config.max_backup_files = 5;
    config.rotate_daily = 0;

    if (hx_clog_init(&config) != HX_CLOG_OK) {
        return 1;
    }

    for (i = 0; i < 20000; ++i) {
        HX_LOG_INFO("rotating line number %d with some padding text", i);
    }

    hx_clog_get_stats(&stats);
    printf("written=%llu rotated_files=%llu\n",
           stats.written_lines, stats.rotated_files);

    hx_clog_shutdown();
    return 0;
}
