/* hx_clog test: size-based rotation produces backups and counts them. */
#include "hx_clog.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dirent.h>
#endif

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while (0)

static int has_prefix(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

static int has_suffix(const char* s, const char* suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    return lf <= ls && strcmp(s + ls - lf, suffix) == 0;
}

static void count_backups(const char* dir, const char* active_name,
                          const char* prefix,
                          int* plain_count, int* gz_count) {
    *plain_count = 0;
    *gz_count = 0;
#if defined(_WIN32)
    {
        char pattern[512];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pattern, sizeof(pattern), "%s\\*", dir);
        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) {
            return;
        }
        do {
            const char* name = fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                strcmp(name, active_name) != 0 &&
                has_prefix(name, prefix)) {
                if (has_suffix(name, ".log.gz")) {
                    (*gz_count)++;
                } else if (has_suffix(name, ".log")) {
                    (*plain_count)++;
                }
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR* d = opendir(dir);
        struct dirent* e;
        if (!d) {
            return;
        }
        while ((e = readdir(d)) != NULL) {
            const char* name = e->d_name;
            if (strcmp(name, active_name) != 0 && has_prefix(name, prefix)) {
                if (has_suffix(name, ".log.gz")) {
                    (*gz_count)++;
                } else if (has_suffix(name, ".log")) {
                    (*plain_count)++;
                }
            }
        }
        closedir(d);
    }
#endif
}

/* Parse the rotation index out of "<prefix><YYYY-MM-DD>.<index>.log[.gz]".
 * Returns -1 if the name does not match that shape. */
static int backup_index(const char* name, const char* prefix) {
    int y, m, d, idx;
    if (!has_prefix(name, prefix)) {
        return -1;
    }
    if (sscanf(name + strlen(prefix), "%d-%d-%d.%d", &y, &m, &d, &idx) == 4) {
        return idx;
    }
    return -1;
}

/* Aggregate index stats over the surviving backups so the test can assert
 * that cleanup kept the *newest* ones (highest indices), not an arbitrary
 * subset chosen by a 1-second-resolution mtime tie. */
typedef struct {
    int overall_min, overall_max; /* across plain + gz */
    int min_plain, max_gz;        /* to check plain are all newer than gz */
    int plain, gz;
} idx_stats;

static void analyze_backups(const char* dir, const char* active_name,
                            const char* prefix, idx_stats* s) {
    s->overall_min = 1 << 30;
    s->overall_max = -1;
    s->min_plain = 1 << 30;
    s->max_gz = -1;
    s->plain = 0;
    s->gz = 0;
#if defined(_WIN32)
    {
        char pattern[512];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pattern, sizeof(pattern), "%s\\*", dir);
        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            const char* name = fd.cFileName;
            int idx, is_gz;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
            if (strcmp(name, active_name) == 0) continue;
            is_gz = has_suffix(name, ".log.gz");
            if (!is_gz && !has_suffix(name, ".log")) continue;
            idx = backup_index(name, prefix);
            if (idx < 0) continue;
            if (idx < s->overall_min) s->overall_min = idx;
            if (idx > s->overall_max) s->overall_max = idx;
            if (is_gz) { s->gz++; if (idx > s->max_gz) s->max_gz = idx; }
            else       { s->plain++; if (idx < s->min_plain) s->min_plain = idx; }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR* dd = opendir(dir);
        struct dirent* e;
        if (!dd) return;
        while ((e = readdir(dd)) != NULL) {
            const char* name = e->d_name;
            int idx, is_gz;
            if (strcmp(name, active_name) == 0) continue;
            is_gz = has_suffix(name, ".log.gz");
            if (!is_gz && !has_suffix(name, ".log")) continue;
            idx = backup_index(name, prefix);
            if (idx < 0) continue;
            if (idx < s->overall_min) s->overall_min = idx;
            if (idx > s->overall_max) s->overall_max = idx;
            if (is_gz) { s->gz++; if (idx > s->max_gz) s->max_gz = idx; }
            else       { s->plain++; if (idx < s->min_plain) s->min_plain = idx; }
        }
        closedir(dd);
    }
#endif
}

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

#if defined(HX_CLOG_ENABLE_ZLIB)
    {
        int plain_count = 0;
        int gz_count = 0;

        hx_clog_config_default(&cfg);
        cfg.log_dir = "./test_rotate_compress_logs";
        cfg.file_name = "compress.log";
        cfg.enable_console = 0;
        cfg.rotate_policy = HX_CLOG_ROTATE_BY_SIZE;
        cfg.max_file_size = 4ULL * 1024ULL;
        cfg.max_backup_files = 2;
        cfg.rotate_daily = 0;

        CHECK(hx_clog_init(&cfg) == HX_CLOG_OK);
        for (i = 0; i < 3000; ++i) {
            HX_LOG_INFO("compress rotation line %d padding padding padding", i);
        }
        hx_clog_flush();
        hx_clog_shutdown();

        count_backups("./test_rotate_compress_logs", "compress.log", "compress.",
                      &plain_count, &gz_count);
        CHECK(plain_count <= 2);
        CHECK(gz_count >= 1);

        /* cleanup must keep the NEWEST backups (highest rotation indices),
         * not an arbitrary subset. With many rotations within the same
         * second, mtime-based ordering would tie and keep the wrong files;
         * indices are the reliable age key. The survivors must therefore be
         * a contiguous block at the top, with every plain backup newer than
         * every compressed one. */
        {
            idx_stats s;
            analyze_backups("./test_rotate_compress_logs", "compress.log",
                            "compress.", &s);
            CHECK(s.overall_max >= 0);
            /* contiguous block of the highest indices */
            CHECK(s.overall_max - s.overall_min + 1 == s.plain + s.gz);
            /* the plain (uncompressed) survivors are the very newest */
            if (s.plain > 0 && s.gz > 0) {
                CHECK(s.min_plain > s.max_gz);
            }
        }
    }
#endif

    return 0;
}
