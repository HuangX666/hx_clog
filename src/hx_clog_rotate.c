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
    int oom;             /* set when a grow allocation failed */
} collect_ctx;

/* Grow the four parallel arrays in `c` to double capacity. Uses explicit
 * malloc+copy+free rather than realloc: with a user allocator hx_clog__realloc
 * frees the old block even on failure, which would dangle these pointers.
 * On failure leaves the existing arrays intact and sets c->oom. */
static void collect_grow(collect_ctx* c) {
    int nc = c->cap * 2;
    char (*nn)[256] = (char (*)[256])hx_clog__malloc((size_t)nc * 256);
    long long* nm = (long long*)hx_clog__malloc((size_t)nc * sizeof(long long));
    long long* nk = (long long*)hx_clog__malloc((size_t)nc * sizeof(long long));
    int* np = (int*)hx_clog__malloc((size_t)nc * sizeof(int));
    if (!nn || !nm || !nk || !np) {
        hx_clog__free(nn);
        hx_clog__free(nm);
        hx_clog__free(nk);
        hx_clog__free(np);
        c->oom = 1;
        return;
    }
    memcpy(nn, c->names, (size_t)c->cap * 256);
    memcpy(nm, c->mtimes, (size_t)c->cap * sizeof(long long));
    memcpy(nk, c->keys, (size_t)c->cap * sizeof(long long));
    memcpy(np, c->compressed, (size_t)c->cap * sizeof(int));
    hx_clog__free(c->names);
    hx_clog__free(c->mtimes);
    hx_clog__free(c->keys);
    hx_clog__free(c->compressed);
    c->names = nn;
    c->mtimes = nm;
    c->keys = nk;
    c->compressed = np;
    c->cap = nc;
}

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

/* Strict archive-name recognizer. A file is a rotation backup only if its
 * whole name is exactly "<prefix>YYYY-MM-DD.<index>[.<ext>][.gz]", where
 * <prefix> is "<stem>." and <index> is a non-negative integer. Matching only
 * the prefix and a trailing ".<ext>" (as an earlier version did) wrongly
 * classified ordinary user files such as "audit.notes.log" as backups and
 * deleted/compressed them — a data-loss bug. Anything that does not parse
 * exactly is left untouched.
 *
 * `prefix` is "<stem>." (with the trailing dot). Sets *compressed to 1 when the
 * name ends in ".gz". */
static int archive_name_match(const char* fname, const char* prefix,
                              const char* ext, int* compressed) {
    size_t plen = strlen(prefix);
    const char* p;
    int y = 0, mo = 0, d = 0, idx = -1, consumed = 0;

    if (compressed) {
        *compressed = 0;
    }
    if (strncmp(fname, prefix, plen) != 0) {
        return 0;
    }
    p = fname + plen;

    /* require "YYYY-MM-DD.<index>" immediately after the prefix */
    if (sscanf(p, "%4d-%2d-%2d.%d%n", &y, &mo, &d, &idx, &consumed) != 4 ||
        consumed <= 0 || idx < 0 ||
        mo < 1 || mo > 12 || d < 1 || d > 31) {
        return 0;
    }
    p += consumed;

    /* whatever follows the index must be exactly the extension (optionally
     * followed by ".gz"), with no extra characters */
    if (!ext || !ext[0]) {
        if (p[0] == '\0') {
            return 1;
        }
        if (strcmp(p, ".gz") == 0) {
            if (compressed) {
                *compressed = 1;
            }
            return 1;
        }
        return 0;
    } else {
        char want[66];
        char wantgz[70];
        snprintf(want, sizeof(want), ".%s", ext);
        snprintf(wantgz, sizeof(wantgz), ".%s.gz", ext);
        if (strcmp(p, want) == 0) {
            return 1;
        }
        if (strcmp(p, wantgz) == 0) {
            if (compressed) {
                *compressed = 1;
            }
            return 1;
        }
        return 0;
    }
}

static void collect_cb(const char* fname, void* user) {
    collect_ctx* c = (collect_ctx*)user;
    char full[HX_CLOG_PATH_MAX];
    int is_compressed = 0;

    if (c->oom) {
        return; /* a prior grow failed; stop collecting */
    }
    if (strcmp(fname, c->active) == 0) {
        return; /* skip active file */
    }
    /* strict match: only real "<stem>.YYYY-MM-DD.<index>[.ext][.gz]" backups,
     * never arbitrary user files that merely share the stem/extension */
    if (!archive_name_match(fname, c->prefix, c->ext, &is_compressed)) {
        return;
    }
    if (c->count >= c->cap) {
        /* grow instead of silently ignoring extra backups, so retention limits
         * still hold for long-running processes with many archives */
        collect_grow(c);
        if (c->oom) {
            return;
        }
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

/* Initial candidate capacity; collect_grow() doubles it on demand so retention
 * is enforced no matter how many archives exist. */
#define HX_ROTATE_INIT_BACKUPS 512

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

    names = (char (*)[256])hx_clog__malloc(HX_ROTATE_INIT_BACKUPS * 256);
    mtimes = (long long*)hx_clog__malloc(HX_ROTATE_INIT_BACKUPS * sizeof(long long));
    keys = (long long*)hx_clog__malloc(HX_ROTATE_INIT_BACKUPS * sizeof(long long));
    compressed = (int*)hx_clog__malloc(HX_ROTATE_INIT_BACKUPS * sizeof(int));
    if (!names || !mtimes || !keys || !compressed) {
        hx_clog__free(names);
        hx_clog__free(mtimes);
        hx_clog__free(keys);
        hx_clog__free(compressed);
        hx_core_report_error(HX_CLOG_ERR_OUT_OF_MEMORY,
                             "rotation cleanup skipped: out of memory allocating "
                             "the backup working set; old backups may accumulate");
        return; /* skip cleanup this round; rotation itself already happened */
    }

    split_name(fs->base_name, stem, sizeof(stem), ext, sizeof(ext));

    memset(&c, 0, sizeof(c));
    snprintf(c.prefix, sizeof(c.prefix), "%s.", stem);
    strncpy(c.ext, ext, sizeof(c.ext) - 1);
    strncpy(c.dir, fs->active_dir, sizeof(c.dir) - 1);
    strncpy(c.active, fs->base_name, sizeof(c.active) - 1);
    c.names = names;
    c.mtimes = mtimes;
    c.keys = keys;
    c.compressed = compressed;
    c.cap = HX_ROTATE_INIT_BACKUPS;
    list_dir(fs->active_dir, collect_cb, &c);

    /* collect_grow() may have reallocated the arrays; re-sync the locals so the
     * sort/cleanup below (and the frees at the end) use the live buffers. */
    names = c.names;
    mtimes = c.mtimes;
    keys = c.keys;
    compressed = c.compressed;
    if (c.oom) {
        hx_core_report_error(HX_CLOG_ERR_OUT_OF_MEMORY,
                             "rotation cleanup: out of memory enumerating backups;"
                             " some old backups may not be cleaned this round");
    }

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
                snprintf(full, sizeof(full), "%s/%s", fs->active_dir, names[i]);
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
                if (compress_or_delete_backup(fs->active_dir, names[i]) == 0) {
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
     * forever; delete the oldest beyond the limit.
     *   max_compressed_files < 0 : unlimited (never delete by count)
     *   max_compressed_files > 0 : that many
     *   max_compressed_files == 0: fall back to max_backup_files */
    {
        int gz_cap;
        if (fs->max_compressed_files < 0) {
            gz_cap = -1; /* unlimited */
        } else if (fs->max_compressed_files > 0) {
            gz_cap = fs->max_compressed_files;
        } else {
            gz_cap = fs->max_backup_files;
        }
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
                                 fs->active_dir, names[i]);
                    } else {
                        snprintf(full, sizeof(full), "%s/%s",
                                 fs->active_dir, names[i]);
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

    /* date-subdir mode: a new day means a new folder, not an in-place
     * archive. Close the current file and reopen — hx_file_open_active()
     * recomputes the dated directory for "today" — leaving yesterday's files
     * untouched in their own folder. Size rotation within the day still falls
     * through to the normal archive path below. */
    if (fs->date_subdir && need_daily) {
        if (fs->fp) {
            fflush(fs->fp);
            fclose(fs->fp);
            fs->fp = NULL;
        }
        if (hx_file_open_active(fs) != HX_CLOG_OK) {
            hx_core_report_error(HX_CLOG_ERR_OPEN_FILE_FAILED,
                                 "date-subdir rollover: opening the new day's "
                                 "log file failed");
            return HX_CLOG_ERR_OPEN_FILE_FAILED;
        }
        fs->current_size = 0;
        hx_core_add_rotated(1);
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
        list_dir(fs->active_dir, index_cb, &ic);
        index = ic.max_index + 1;
        make_archive_name(archive, sizeof(archive), stem, ext, &cur, index);
    }

    /* flush + close current */
    if (fs->fp) {
        fflush(fs->fp);
        fclose(fs->fp);
        fs->fp = NULL;
    }

    snprintf(archive_full, sizeof(archive_full), "%s/%s", fs->active_dir, archive);
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
    list_dir(fs->active_dir, index_cb, &ic);
    index = ic.max_index + 1;
    make_archive_name(archive, sizeof(archive), stem, ext, &tmv, index);

    fflush(fs->fp);
    fclose(fs->fp);
    fs->fp = NULL;

    snprintf(archive_full, sizeof(archive_full), "%s/%s", fs->active_dir, archive);
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
