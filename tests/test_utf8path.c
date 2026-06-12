/* hx_clog test: UTF-8 (non-ASCII) log directory and file name.
 *
 * Paths handed to the library are UTF-8 by convention. On Windows the
 * implementation must convert them to UTF-16 and use the wide APIs; with the
 * old narrow (ANSI) calls a Chinese directory name would be mangled or fail
 * outright. The byte strings below spell "logs_日志" and "应用.log" and are
 * written as explicit escape sequences so no editor/codepage can corrupt
 * them.
 */
#include "hx_clog.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static long long path_size(const char* utf8) {
    wchar_t w[1024];
    WIN32_FILE_ATTRIBUTE_DATA ad;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 1024) <= 0) {
        return -1;
    }
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &ad)) {
        return -1;
    }
    return ((long long)ad.nFileSizeHigh << 32) | (long long)ad.nFileSizeLow;
}
#else
#  include <sys/stat.h>
static long long path_size(const char* utf8) {
    struct stat st;
    if (stat(utf8, &st) != 0) {
        return -1;
    }
    return (long long)st.st_size;
}
#endif

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while (0)

/* "./test_utf8_logs/logs_日志" */
static const char k_dir[] =
    "./test_utf8_logs/logs_\xE6\x97\xA5\xE5\xBF\x97";
/* "应用.log" */
static const char k_file[] = "\xE5\xBA\x94\xE7\x94\xA8.log";

int main(void) {
    hx_clog_config_t cfg;
    hx_clog_stats_t stats;
    char active_path[1024];
    int i;

    hx_clog_config_default(&cfg);
    cfg.enable_console = 0;
    cfg.enable_file = 1;
    cfg.log_dir = k_dir;
    cfg.file_name = k_file;
    cfg.level = HX_CLOG_LEVEL_TRACE;
    cfg.rotate_policy = HX_CLOG_ROTATE_BY_SIZE;
    cfg.max_file_size = 512; /* tiny: force a rotation in the UTF-8 dir too */
    cfg.max_backup_files = 2;
    cfg.rotate_daily = 0;
    CHECK(hx_clog_init(&cfg) == HX_CLOG_OK);

    for (i = 0; i < 40; ++i) {
        HX_LOG_INFO("utf8 path line %d - some padding to grow the file", i);
    }
    hx_clog_flush();

    CHECK(hx_clog_get_stats(&stats) == HX_CLOG_OK);
    CHECK(stats.written_lines >= 40);
    CHECK(stats.rotated_files >= 1); /* archive rename worked in UTF-8 dir */

    snprintf(active_path, sizeof(active_path), "%s/%s", k_dir, k_file);
    CHECK(path_size(active_path) > 0); /* file really exists under that name */

    hx_clog_shutdown();
    printf("test_utf8path: OK\n");
    return 0;
}
