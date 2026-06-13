/* hx_clog test: date_subdir writes logs under a per-day YYYY-MM-DD folder. */
#include "hx_clog.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  include <windows.h>
static int path_exists(const char* utf8) {
    wchar_t w[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 1024) <= 0) return 0;
    return GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
}
static int count_in_dir(const char* dir, const char* prefix) {
    char pattern[1024];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0;
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
            strncmp(fd.cFileName, prefix, strlen(prefix)) == 0) {
            n++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}
#else
#  include <sys/stat.h>
#  include <dirent.h>
static int path_exists(const char* p) {
    struct stat st;
    return stat(p, &st) == 0;
}
static int count_in_dir(const char* dir, const char* prefix) {
    DIR* d = opendir(dir);
    struct dirent* e;
    int n = 0;
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, prefix, strlen(prefix)) == 0) n++;
    }
    closedir(d);
    return n;
}
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
    char today[16];
    char day_dir[256];
    char active_path[300];
    time_t t;
    struct tm tmv;
    int i;

    t = time(NULL);
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    snprintf(today, sizeof(today), "%04d-%02d-%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    snprintf(day_dir, sizeof(day_dir), "./test_datedir_logs/%s", today);
    snprintf(active_path, sizeof(active_path), "%s/app.log", day_dir);

    hx_clog_config_default(&cfg);
    cfg.log_dir = "./test_datedir_logs";
    cfg.file_name = "app.log";
    cfg.enable_console = 0;
    cfg.level = HX_CLOG_LEVEL_TRACE;
    cfg.date_subdir = 1;                     /* the feature under test */
    cfg.rotate_policy = HX_CLOG_ROTATE_BY_SIZE;
    cfg.max_file_size = 4ULL * 1024ULL;      /* force a few rotations */
    cfg.rotate_daily = 0;
    cfg.max_backup_files = 2;                 /* keep 2 plain, compress older */

    CHECK(hx_clog_init(&cfg) == HX_CLOG_OK);
    for (i = 0; i < 600; ++i) {
        HX_LOG_INFO("dated line %d padding padding padding padding", i);
    }
    hx_clog_flush();

    /* the active file must live under ./test_datedir_logs/<today>/app.log */
    CHECK(path_exists(day_dir));
    CHECK(path_exists(active_path));

    CHECK(hx_clog_get_stats(&stats) == HX_CLOG_OK);
    CHECK(stats.written_lines >= 600);
    CHECK(stats.rotated_files >= 1);

    hx_clog_shutdown();

    /* rotated archives must also be inside the dated folder, not the base */
    CHECK(count_in_dir(day_dir, "app.") >= 2);
    /* the base dir must contain only the date folder, no stray app.log */
    {
        char base_active[64];
        snprintf(base_active, sizeof(base_active), "./test_datedir_logs/app.log");
        CHECK(!path_exists(base_active));
    }

    printf("test_date_subdir: OK\n");
    return 0;
}
