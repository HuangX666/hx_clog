/* hx_clog test: startup and interval time rotation. */
#include "hx_clog.h"

#include <stdio.h>

#if defined(_WIN32)
#  include <direct.h>
#  include <windows.h>
static void sleep_ms(unsigned int ms) { Sleep(ms); }
static void mkdir_one(const char* p) { _mkdir(p); }
#else
#  include <sys/stat.h>
#  include <unistd.h>
static void sleep_ms(unsigned int ms) { usleep(ms * 1000); }
static void mkdir_one(const char* p) { mkdir(p, 0755); }
#endif

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while (0)

int main(void) {
    hx_clog_config_t cfg;
    hx_clog_stats_t stats;
    unsigned long long rotated_after_startup;
    FILE* fp;

    mkdir_one("./test_rotate_time_logs");
    fp = fopen("./test_rotate_time_logs/startup.log", "wb");
    CHECK(fp != NULL);
    fputs("old active file\n", fp);
    fclose(fp);

    hx_clog_config_default(&cfg);
    cfg.log_dir = "./test_rotate_time_logs";
    cfg.file_name = "startup.log";
    cfg.enable_console = 0;
    cfg.rotate_policy = HX_CLOG_ROTATE_NONE;
    cfg.rotate_daily = 0;
    cfg.rotate_on_startup = 1;
    CHECK(hx_clog_init(&cfg) == HX_CLOG_OK);
    HX_LOG_INFO("new active file");
    hx_clog_flush();
    CHECK(hx_clog_get_stats(&stats) == HX_CLOG_OK);
    CHECK(stats.rotated_files >= 1);
    rotated_after_startup = stats.rotated_files;
    hx_clog_shutdown();

    hx_clog_config_default(&cfg);
    cfg.log_dir = "./test_rotate_time_logs";
    cfg.file_name = "interval.log";
    cfg.enable_console = 0;
    cfg.rotate_policy = HX_CLOG_ROTATE_BY_TIME;
    cfg.rotate_daily = 0;
    cfg.rotate_interval_seconds = 1;
    cfg.max_backup_files = 8;
    CHECK(hx_clog_init(&cfg) == HX_CLOG_OK);
    HX_LOG_INFO("before interval");
    hx_clog_flush();
    sleep_ms(1200);
    HX_LOG_INFO("after interval");
    hx_clog_flush();
    CHECK(hx_clog_get_stats(&stats) == HX_CLOG_OK);
    CHECK(stats.rotated_files > rotated_after_startup);
    hx_clog_shutdown();

    printf("test_rotate_time: OK (rotated=%llu)\n", stats.rotated_files);
    return 0;
}
