/* hx_clog test: size-based rotation produces backups and counts them. */
#include "hx_clog.h"

#include <stdio.h>

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while (0)

int main(void) {
    hx_clog_config_t cfg;
    hx_clog_stats_t stats;
    int i;

    hx_clog_config_default(&cfg);
    cfg.log_dir = "./test_rotate_logs";
    cfg.file_name = "rot.log";
    cfg.enable_console = 0;
    cfg.rotate_policy = HX_CLOG_ROTATE_BY_SIZE;
    cfg.max_file_size = 16ULL * 1024ULL; /* 16 KB -> rotates fast */
    cfg.max_backup_files = 3;
    cfg.rotate_daily = 0;

    CHECK(hx_clog_init(&cfg) == HX_CLOG_OK);

    for (i = 0; i < 5000; ++i) {
        HX_LOG_INFO("rotation test line %d padding padding padding", i);
    }

    hx_clog_flush();
    CHECK(hx_clog_get_stats(&stats) == HX_CLOG_OK);
    CHECK(stats.written_lines >= 5000);
    CHECK(stats.rotated_files >= 1);

    hx_clog_shutdown();
    printf("test_rotate: OK (rotated=%llu)\n", stats.rotated_files);
    return 0;
}
