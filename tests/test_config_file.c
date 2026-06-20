/* hx_clog test: INI config file loading (hx_clog_init_from_file). */
#include "hx_clog.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); return 1; } \
} while (0)

int main(void) {
    const char* ini = "test_config_file.ini";
    hx_clog_config_t got;
    FILE* f = fopen(ini, "wb");
    CHECK(f != NULL);
    fputs("# hx_clog test config\n"
          "[hx_clog]\n"
          "level = warn\n"
          "console = 0\n"
          "file_output = 0\n"
          "mode = sync\n"
          "max_file_size = 2M\n"
          "max_backup_files = 5\n"
          "rotate_daily = true\n"
          "pattern = %v%n\n"
          "logger = svc\n"
          "; a comment line\n"
          "overflow = drop_new\n", f);
    fclose(f);

    CHECK(hx_clog_init_from_file(ini) == HX_CLOG_OK);

    memset(&got, 0, sizeof(got));
    CHECK(hx_clog_get_config(&got) == HX_CLOG_OK);
    CHECK(got.level == HX_CLOG_LEVEL_WARN);
    CHECK(got.enable_console == 0);
    CHECK(got.enable_file == 0);
    CHECK(got.max_file_size == 2ull * 1024 * 1024);   /* "2M" */
    CHECK(got.max_backup_files == 5);
    CHECK(got.rotate_daily == 1);
    CHECK(got.overflow_policy == HX_CLOG_OVERFLOW_DROP_NEW);
    CHECK(strcmp(got.pattern, "%v%n") == 0);
    CHECK(strcmp(got.logger_name, "svc") == 0);

    hx_clog_shutdown();
    remove(ini);

    /* missing file is a clean error, not a crash */
    CHECK(hx_clog_init_from_file("does_not_exist_12345.ini")
              == HX_CLOG_ERR_OPEN_FILE_FAILED);

    printf("ALL PASS\n");
    return 0;
}
