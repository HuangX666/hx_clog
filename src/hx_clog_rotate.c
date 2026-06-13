/*
 * hx_clog - log rotation and cleanup.
 *
 * Operates on the file sink state. Two triggers:
 *   - by size: active file would exceed max_file_size.
 *   - by day:  the calendar day changed since the file was opened.
 *
 * Archive naming: <stem>.<YYYY-MM-DD>.<N>.<ext>, e.g. app.2026-06-07.1.log
 *
 * Cleanup order (per design doc):
 *   1. delete files older than max_backup_days.
 *   2. compress oldest uncompressed files beyond max_backup_files.
 */
#include "hx_clog_file.h"

#if defined(HX_CLOG_ENABLE_ZLIB)
#  include <zlib.h>
#endif

/* Split "app.log" into stem="app" and ext="log" (ext may be empty). */
static void split_name(const char* name, char* stem, unsigned int stem_cap,
                       char* ext, unsigned int ext_cap) {
    const char* dot = NULL;
    const char* p;
    unsigned int i = 0;
    for (p = name; *p; ++p) {
        if (*p == '.') {
            dot = p;
        }
    }
    if (!dot || dot == name) {
        /* no extension */
        for (p = name; *p && i + 1 < stem_cap; ++p) {
            stem[i++] = *p;
        }
        stem[i] = '\0';
        if (ext_cap) ext[0] = '\0';
        return;
    }
    for (p = name; p < dot && i + 1 < stem_cap; ++p) {
        stem[i++] = *p;
    }
    stem[i] = '\0';
    i = 0;
    for (p = dot + 1; *p && i + 1 < ext_cap; ++p) {
        ext[i++] = *p;
    }
    ext[i] = '\0';
}

static void make_archive_name(char* out, unsigned int cap,
                              const char* stem, const char* ext,
                              const struct tm* tmv, int index) {
    /* stem.YYYY-MM-DD.index[.ext] */
    if (ext && ext[0]) {
        snprintf(out, cap, "%s.%04d-%02d-%02d.%d.%s",
                 stem, tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday,
                 index, ext);
    } else {
        snprintf(out, cap, "%s.%04d-%02d-%02d.%d",
                 stem, tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday,
                 index);
    }
}

/* ---- directory enumeration ---- */

typedef void (*dir_cb)(const char* fname, void* user);

#if !defined(HX_PLATFORM_WINDOWS)
#include <dirent.h>
#endif

static void list_dir(const char* dir, dir_cb cb, void* user) {
#if defined(HX_PLATFORM_WINDOWS)
    /* paths are UTF-8 by convention; use the wide API so non-ASCII log
     * directories work regardless of the ANSI codepage */
    char pattern[HX_CLOG_PATH_MAX];
    wchar_t wpattern[HX_CLOG_PATH_MAX];
    char name_utf8[512];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    if (hx_utf8_to_wide(pattern, wpattern, HX_CLOG_PATH_MAX) < 0) {
        return;
    }
    h = FindFirstFileW(wpattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (hx_wide_to_utf8(fd.cFileName, name_utf8, sizeof(name_utf8)) > 0) {
                cb(name_utf8, user);
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir);
    struct dirent* e;
    if (!d) {
        return;
    }
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' &&
            (e->d_name[1] == '\0' ||
             (e->d_name[1] == '.' && e->d_name[2] == '\0'))) {
            continue;
        }
        cb(e->d_name, user);
    }
    closedir(d);
#endif
}

/* ---- collect backups matching "<stem>." prefix and ext ---- */

typedef struct {
    char  prefix[256];   /* "<stem>." */
    char  ext[64];
    char  dir[HX_CLOG_PATH_MAX];
    char  active[256];   /* base_name, never delete */

    char (*names)[256];
    long long* mtimes;
    long long* keys;     /* age-ordering key parsed from the name (date+index) */
    int* compressed;
    int count;
    int cap;
} collect_ctx;

/* Derive a monotonic age key from an archive file name of the form
 * "<stem>.YYYY-MM-DD.<index>[.ext][.gz]". The embedded date + index is the
 * exact rotation order and, unlike the file mtime (1-second resolution),
 * stays correct even when many rotations happen within the same second.
 * Returns a key that sorts oldest-first, or `fallback` (the mtime) when the
 * name cannot be parsed. */
static long long archive_order_key(const char* fname, const char* prefix,
                                   long long fallback) {
    const char* after = fname + strlen(prefix);
    int y = 0, mo = 0, d = 0, idx = -1;
    if (sscanf(after, "%4d-%2d-%2d.%d", &y, &mo, &d, &idx) == 4 && idx >= 0) {
        long long date_num = (long long)y * 10000 + (long long)mo * 100 + d;
        return date_num * 100000000LL + (long long)idx;
    }
    return fallback;
}

static int has_prefix(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

static int has_suffix(const char* s, const char* suffix) {
    size_t ls = strlen(s), lsuf = strlen(suffix);
    if (lsuf == 0) {
        return 1;
    }
    if (lsuf > ls) {
        return 0;
    }
    return strcmp(s + (ls - lsuf), suffix) == 0;
}

static int archive_suffix_match(const char* fname, const char* ext, int* compressed) {
    char extdot[66];
    char extgz[70];

    if (compressed) {
        *compressed = 0;
    }

    if (!ext || !ext[0]) {
        if (has_suffix(fname, ".gz")) {
            if (compressed) {
                *compressed = 1;
            }
        }
        return 1;
    }

    snprintf(extdot, sizeof(extdot), ".%s", ext);
    snprintf(extgz, sizeof(extgz), ".%s.gz", ext);
    if (has_suffix(fname, extdot)) {
        return 1;
    }
    if (has_suffix(fname, extgz)) {
        if (compressed) {
            *compressed = 1;
        }
        return 1;
    }
    return 0;
}

static void collect_cb(const char* fname, void* user) {
    collect_ctx* c = (collect_ctx*)user;
    char full[HX_CLOG_PATH_MAX];
    int is_compressed = 0;

    if (strcmp(fname, c->active) == 0) {
        return; /* skip active file */
    }
    if (!has_prefix(fname, c->prefix)) {
        return;
    }
    if (!archive_suffix_match(fname, c->ext, &is_compressed)) {
        return;
    }
    if (c->count >= c->cap) {
        return;
    }
    snprintf(full, sizeof(full), "%s/%s", c->dir, fname);
    strncpy(c->names[c->count], fname, 255);
    c->names[c->count][255] = '\0';
    {
        long long mt = 0;
#if defined(HX_PLATFORM_WINDOWS)
        wchar_t wfull[HX_CLOG_PATH_MAX];
        WIN32_FILE_ATTRIBUTE_DATA ad;
        if (hx_utf8_to_wide(full, wfull, HX_CLOG_PATH_MAX) > 0 &&
            GetFileAttributesExW(wfull, GetFileExInfoStandard, &ad)) {
            ULARGE_INTEGER u;
            u.LowPart = ad.ftLastWriteTime.dwLowDateTime;
            u.HighPart = ad.ftLastWriteTime.dwHighDateTime;
            mt = (long long)(u.QuadPart / 10000000ULL);
        }
#else
        struct stat st;
        if (stat(full, &st) == 0) {
            mt = (long long)st.st_mtime;
        }
#endif
        c->mtimes[c->count] = mt;
        if (c->keys) {
            c->keys[c->count] = archive_order_key(fname, c->prefix, mt);
        }
    }
    if (c->compressed) {
        c->compressed[c->count] = is_compressed;
    }
    c->count++;
}

#if defined(HX_CLOG_ENABLE_ZLIB)
static gzFile hx_gzopen_write(const char* path) {
#if defined(HX_PLATFORM_WINDOWS)
    wchar_t wpath[HX_CLOG_PATH_MAX];
    if (hx_utf8_to_wide(path, wpath, HX_CLOG_PATH_MAX) < 0) {
        return NULL;
    }
    return gzopen_w(wpath, "wb");
#else
    return gzopen(path, "wb");
#endif
}

static int gzip_file(const char* src, const char* dst) {
    FILE* in;
    gzFile out;
    char buf[16 * 1024];
    int ok = 1;

    in = hx_fopen(src, "rb");
    if (!in) {
        return -1;
    }
    hx_remove(dst);
    out = hx_gzopen_write(dst);
    if (!out) {
        fclose(in);
        return -1;
    }
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), in);
        if (n > 0) {
            if (gzwrite(out, buf, (unsigned int)n) != (int)n) {
                ok = 0;
                break;
            }
        }
        if (n < sizeof(buf)) {
            if (ferror(in)) {
                ok = 0;
            }
            break;
        }
    }
    if (gzclose(out) != Z_OK) {
        ok = 0;
    }
    fclose(in);
    if (!ok) {
        hx_remove(dst);
        return -1;
    }
    return hx_remove(src);
}
#endif

static int compress_or_delete_backup(const char* dir, const char* name) {
    char src[HX_CLOG_PATH_MAX];
    char dst[HX_CLOG_PATH_MAX];

    snprintf(src, sizeof(src), "%s/%s", dir, name);
#if defined(HX_CLOG_ENABLE_ZLIB)
    snprintf(dst, sizeof(dst), "%s/%s.gz", dir, name);
    return gzip_file(src, dst);
#else
    (void)dst;
    return hx_remove(src);
#endif
}

/* Highest existing archive index for today's date (to pick the next one). */
typedef struct {
    char date_tag[16]; /* "YYYY-MM-DD" */
    char stem[256];
    int  max_index;
} index_ctx;

static void index_cb(const char* fname, void* user) {
    index_ctx* ic = (index_ctx*)user;
    char needle[300];
    const char* after;
    int idx;
    snprintf(needle, sizeof(needle), "%s.%s.", ic->stem, ic->date_tag);
    if (!has_prefix(fname, needle)) {
        return;
    }
    after = fname + strlen(needle);
    idx = atoi(after);
    if (idx > ic->max_index) {
        ic->max_index = idx;
    }
}

#define HX_ROTATE_MAX_BACKUPS 512

void hx_rotate_cleanup(struct hx_file_sink_impl* fs) {
    /* heap-allocated working set (~140 KB): a static buffer would not be
     * thread-safe with more than one file sink, and would stay resident
     * forever even when rotation never happens */
    char (*names)[256];
    long long* mtimes;
    long long* keys;   /* age-ordering key (date+index from the name) */
    int* compressed;   /* 0 plain, 1 = .gz on disk, 2 = compressed just now
                        * (name lacks the .gz suffix) */
    collect_ctx c;
    char stem[256], ext[64];
    int i, j;

    names = (char (*)[256])hx_clog__malloc(HX_ROTATE_MAX_BACKUPS * 256);
    mtimes = (long long*)hx_clog__malloc(HX_ROTATE_MAX_BACKUPS * sizeof(long long));
    keys = (long long*)hx_clog__malloc(HX_ROTATE_MAX_BACKUPS * sizeof(long long));
    compressed = (int*)hx_clog__malloc(HX_ROTATE_MAX_BACKUPS * sizeof(int));
    if (!names || !mtimes || !keys || !compressed) {
        hx_clog__free(names);
        hx_clog__free(mtimes);
        hx_clog__free(keys);
        hx_clog__free(compressed);
        return; /* skip cleanup this round; rotation itself already happened */
    }

    split_name(fs->base_name, stem, sizeof(stem), ext, sizeof(ext));

    memset(&c, 0, sizeof(c));
    snprintf(c.prefix, sizeof(c.prefix), "%s.", stem);
    strncpy(c.ext, ext, sizeof(c.ext) - 1);
    strncpy(c.dir, fs->dir, sizeof(c.dir) - 1);
    strncpy(c.active, fs->base_name, sizeof(c.active) - 1);
    c.names = names;
    c.mtimes = mtimes;
    c.keys = keys;
    c.compressed = compressed;
    c.cap = HX_ROTATE_MAX_BACKUPS;
    list_dir(fs->dir, collect_cb, &c);

    /* sort ascending (oldest first) by the parsed date+index key, which is
     * exact per rotation; the file mtime has only 1-second resolution and
     * ties under rapid rotation, which would keep/compress the wrong files */
    for (i = 1; i < c.count; ++i) {
        long long key = keys[i];
        long long mt = mtimes[i];
        int comp = compressed[i];
        char tmp[256];
        strncpy(tmp, names[i], sizeof(tmp));
        j = i - 1;
        while (j >= 0 && keys[j] > key) {
            keys[j + 1] = keys[j];
            mtimes[j + 1] = mtimes[j];
            compressed[j + 1] = compressed[j];
            strncpy(names[j + 1], names[j], 256);
            --j;
        }
        keys[j + 1] = key;
        mtimes[j + 1] = mt;
        compressed[j + 1] = comp;
        strncpy(names[j + 1], tmp, 256);
    }

    /* 1. delete by age */
    if (fs->max_backup_days > 0) {
        long long now = (long long)time(NULL);
        long long cutoff = now - (long long)fs->max_backup_days * 86400LL;
        for (i = 0; i < c.count; ++i) {
            if (mtimes[i] > 0 && mtimes[i] < cutoff) {
                char full[HX_CLOG_PATH_MAX];
                snprintf(full, sizeof(full), "%s/%s", fs->dir, names[i]);
                hx_remove(full);
                names[i][0] = '\0'; /* mark removed */
            }
        }
    }

    /* compact remaining */
    {
        int n = 0;
        for (i = 0; i < c.count; ++i) {
            if (names[i][0]) {
                if (n != i) {
                    strncpy(names[n], names[i], 256);
                    mtimes[n] = mtimes[i];
                    keys[n] = keys[i];
                    compressed[n] = compressed[i];
                }
                ++n;
            }
        }
        c.count = n;
    }

    /* 2. keep max_backup_files recent plain backups; compress older extras
     * (or delete them when zlib support is unavailable). */
    if (fs->max_backup_files > 0) {
        int plain_count = 0;
        int to_compress;
        for (i = 0; i < c.count; ++i) {
            if (!compressed[i]) {
                plain_count++;
            }
        }
        to_compress = plain_count - fs->max_backup_files;
        for (i = 0; i < c.count && to_compress > 0; ++i) {
            if (!compressed[i] && names[i][0]) {
                if (compress_or_delete_backup(fs->dir, names[i]) == 0) {
#if defined(HX_CLOG_ENABLE_ZLIB)
                    compressed[i] = 2; /* now lives on disk as <name>.gz */
#else
                    names[i][0] = '\0'; /* deleted */
#endif
                    to_compress--;
                }
            }
        }
    }

    /* 3. cap the number of compressed backups so .gz files cannot pile up
     * forever; delete the oldest beyond the limit. */
    {
        int gz_cap = fs->max_compressed_files > 0 ? fs->max_compressed_files
                                                  : fs->max_backup_files;
        if (gz_cap > 0) {
            int gz_count = 0;
            for (i = 0; i < c.count; ++i) {
                if (names[i][0] && compressed[i]) {
                    gz_count++;
                }
            }
            for (i = 0; i < c.count && gz_count > gz_cap; ++i) {
                if (names[i][0] && compressed[i]) {
                    char full[HX_CLOG_PATH_MAX];
                    if (compressed[i] == 2) {
                        snprintf(full, sizeof(full), "%s/%s.gz",
                                 fs->dir, names[i]);
                    } else {
                        snprintf(full, sizeof(full), "%s/%s",
                                 fs->dir, names[i]);
                    }
                    if (hx_remove(full) == 0) {
                        names[i][0] = '\0';
                        gz_count--;
                    }
                }
            }
        }
    }

    hx_clog__free(names);
    hx_clog__free(mtimes);
    hx_clog__free(keys);
    hx_clog__free(compressed);
}

/* Decide whether rotation is needed and perform it. Caller holds fs->lock. */
int hx_rotate_maybe(struct hx_file_sink_impl* fs, unsigned int incoming) {
    hx_timestamp_t ts;
    struct tm tmv;
    int need_size = 0;
    int need_time = 0;
    int need_daily = 0;
    int need_interval = 0;
    char stem[256], ext[64];
    char date_tag[16];
    char archive[HX_CLOG_PATH_MAX];
    char archive_full[HX_CLOG_PATH_MAX];
    index_ctx ic;
    int index;

    hx_now(&ts);
    hx_localtime(ts.sec, &tmv);

    /* a failing rotation (e.g. archive locked by another process) is retried
     * at most once per second, not once per log line */
    if (fs->rotate_fail_sec != 0 && ts.sec <= fs->rotate_fail_sec) {
        return HX_CLOG_OK;
    }

    if ((fs->policy == HX_CLOG_ROTATE_BY_SIZE ||
         fs->policy == HX_CLOG_ROTATE_BY_SIZE_AND_TIME) &&
        fs->max_file_size > 0) {
        if (fs->current_size + incoming > fs->max_file_size &&
            fs->current_size > 0) {
            need_size = 1;
        }
    }

    if (fs->rotate_daily ||
        fs->policy == HX_CLOG_ROTATE_BY_TIME ||
        fs->policy == HX_CLOG_ROTATE_BY_SIZE_AND_TIME) {
        if (tmv.tm_yday != fs->cur_yday || tmv.tm_year != fs->cur_year) {
            need_time = 1;
            need_daily = 1;
        }
    }

    if ((fs->policy == HX_CLOG_ROTATE_BY_TIME ||
         fs->policy == HX_CLOG_ROTATE_BY_SIZE_AND_TIME) &&
        fs->rotate_interval_seconds > 0 &&
        fs->current_size > 0) {
        if (fs->rotate_align) {
            /* wall-clock aligned: rotate when the open time and now fall in
             * different interval buckets (e.g. 3600 -> on the hour, UTC
             * aligned) */
            long long bucket_now = (long long)ts.sec /
                                   (long long)fs->rotate_interval_seconds;
            long long bucket_open = (long long)fs->opened_sec /
                                    (long long)fs->rotate_interval_seconds;
            if (bucket_now != bucket_open) {
                need_time = 1;
                need_interval = 1;
            }
        } else if (ts.sec >= fs->opened_sec +
                             (time_t)fs->rotate_interval_seconds) {
            need_time = 1;
            need_interval = 1;
        }
    }

    if (!need_size && !need_time) {
        return HX_CLOG_OK;
    }

    /* Use the date the *current* file belongs to for the archive tag. */
    {
        struct tm cur;
        /* the active file's content started when it was opened, so its
         * recorded open time gives the correct archive date even when the
         * process sat idle across several days (a fixed "now - 1 day" would
         * mislabel such files) */
        cur = tmv;
        if (need_time && fs->opened_sec > 0) {
            hx_localtime(fs->opened_sec, &cur);
        }
        (void)need_daily;
        (void)need_interval;
        snprintf(date_tag, sizeof(date_tag), "%04d-%02d-%02d",
                 cur.tm_year + 1900, cur.tm_mon + 1, cur.tm_mday);
        memset(&ic, 0, sizeof(ic));
        split_name(fs->base_name, stem, sizeof(stem), ext, sizeof(ext));
        strncpy(ic.stem, stem, sizeof(ic.stem) - 1);
        strncpy(ic.date_tag, date_tag, sizeof(ic.date_tag) - 1);
        ic.max_index = 0;
        list_dir(fs->dir, index_cb, &ic);
        index = ic.max_index + 1;
        make_archive_name(archive, sizeof(archive), stem, ext, &cur, index);
    }

    /* flush + close current */
    if (fs->fp) {
        fflush(fs->fp);
        fclose(fs->fp);
        fs->fp = NULL;
    }

    snprintf(archive_full, sizeof(archive_full), "%s/%s", fs->dir, archive);
    if (hx_rename(fs->active_path, archive_full) != 0) {
        /* archive failed (file locked, permissions): keep appending to the
         * un-archived active file. hx_file_open_active() recomputes the real
         * current_size, so the size trigger stays correct. */
        int first_failure = !fs->rename_failing;
        fs->rename_failing = 1;
        fs->rotate_fail_sec = ts.sec;
        if (hx_file_open_active(fs) != HX_CLOG_OK) {
            hx_core_report_error(HX_CLOG_ERR_OPEN_FILE_FAILED,
                                 "rotation: reopening the active log file failed");
            return HX_CLOG_ERR_OPEN_FILE_FAILED;
        }
        if (first_failure) {
            hx_core_report_error(HX_CLOG_ERR_PLATFORM,
                                 "rotation: archiving the active log file failed"
                                 " (locked by another process or no permission)");
        }
        return HX_CLOG_ERR_PLATFORM;
    }
    fs->rename_failing = 0;
    fs->rotate_fail_sec = 0;

    /* reopen fresh active file */
    if (hx_file_open_active(fs) != HX_CLOG_OK) {
        hx_core_report_error(HX_CLOG_ERR_OPEN_FILE_FAILED,
                             "rotation: reopening the active log file failed");
        return HX_CLOG_ERR_OPEN_FILE_FAILED;
    }
    fs->current_size = 0;

    hx_core_add_rotated(1);
    hx_rotate_cleanup(fs);
    return HX_CLOG_OK;
}

int hx_rotate_force(struct hx_file_sink_impl* fs) {
    hx_timestamp_t ts;
    struct tm tmv;
    char stem[256], ext[64];
    char archive[HX_CLOG_PATH_MAX];
    char archive_full[HX_CLOG_PATH_MAX];
    char date_tag[16];
    index_ctx ic;
    int index;

    if (!fs || !fs->fp || fs->current_size == 0) {
        return HX_CLOG_OK;
    }

    ts.sec = fs->opened_sec > 0 ? fs->opened_sec : time(NULL);
    ts.msec = 0;
    hx_localtime(ts.sec, &tmv);
    snprintf(date_tag, sizeof(date_tag), "%04d-%02d-%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);

    memset(&ic, 0, sizeof(ic));
    split_name(fs->base_name, stem, sizeof(stem), ext, sizeof(ext));
    strncpy(ic.stem, stem, sizeof(ic.stem) - 1);
    strncpy(ic.date_tag, date_tag, sizeof(ic.date_tag) - 1);
    ic.max_index = 0;
    list_dir(fs->dir, index_cb, &ic);
    index = ic.max_index + 1;
    make_archive_name(archive, sizeof(archive), stem, ext, &tmv, index);

    fflush(fs->fp);
    fclose(fs->fp);
    fs->fp = NULL;

    snprintf(archive_full, sizeof(archive_full), "%s/%s", fs->dir, archive);
    if (hx_rename(fs->active_path, archive_full) != 0) {
        int first_failure = !fs->rename_failing;
        fs->rename_failing = 1;
        fs->rotate_fail_sec = time(NULL); /* ts.sec holds the open time here */
        if (hx_file_open_active(fs) != HX_CLOG_OK) {
            hx_core_report_error(HX_CLOG_ERR_OPEN_FILE_FAILED,
                                 "rotation: reopening the active log file failed");
            return HX_CLOG_ERR_OPEN_FILE_FAILED;
        }
        if (first_failure) {
            hx_core_report_error(HX_CLOG_ERR_PLATFORM,
                                 "rotation: archiving the active log file failed"
                                 " (locked by another process or no permission)");
        }
        return HX_CLOG_ERR_PLATFORM;
    }
    fs->rename_failing = 0;
    fs->rotate_fail_sec = 0;

    if (hx_file_open_active(fs) != HX_CLOG_OK) {
        hx_core_report_error(HX_CLOG_ERR_OPEN_FILE_FAILED,
                             "rotation: reopening the active log file failed");
        return HX_CLOG_ERR_OPEN_FILE_FAILED;
    }
    fs->current_size = 0;

    hx_core_add_rotated(1);
    hx_rotate_cleanup(fs);
    return HX_CLOG_OK;
}
