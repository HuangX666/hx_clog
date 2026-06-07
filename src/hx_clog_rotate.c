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
 *   2. delete oldest files beyond max_backup_files.
 */
#include "hx_clog_file.h"

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
    char pattern[HX_CLOG_PATH_MAX];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            cb(fd.cFileName, user);
        }
    } while (FindNextFileA(h, &fd));
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
    int count;
    int cap;
} collect_ctx;

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

static void collect_cb(const char* fname, void* user) {
    collect_ctx* c = (collect_ctx*)user;
    char full[HX_CLOG_PATH_MAX];
    char extdot[66];

    if (strcmp(fname, c->active) == 0) {
        return; /* skip active file */
    }
    if (!has_prefix(fname, c->prefix)) {
        return;
    }
    if (c->ext[0]) {
        snprintf(extdot, sizeof(extdot), ".%s", c->ext);
        if (!has_suffix(fname, extdot)) {
            return;
        }
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
        WIN32_FILE_ATTRIBUTE_DATA ad;
        if (GetFileAttributesExA(full, GetFileExInfoStandard, &ad)) {
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
    }
    c->count++;
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

void hx_rotate_cleanup(struct hx_file_sink_impl* fs) {
    static char names[512][256];
    static long long mtimes[512];
    collect_ctx c;
    char stem[256], ext[64];
    int i, j;

    split_name(fs->base_name, stem, sizeof(stem), ext, sizeof(ext));

    memset(&c, 0, sizeof(c));
    snprintf(c.prefix, sizeof(c.prefix), "%s.", stem);
    strncpy(c.ext, ext, sizeof(c.ext) - 1);
    strncpy(c.dir, fs->dir, sizeof(c.dir) - 1);
    strncpy(c.active, fs->base_name, sizeof(c.active) - 1);
    c.names = names;
    c.mtimes = mtimes;
    c.cap = 512;
    list_dir(fs->dir, collect_cb, &c);

    /* sort by mtime ascending (oldest first), simple insertion sort */
    for (i = 1; i < c.count; ++i) {
        long long mt = mtimes[i];
        char tmp[256];
        strncpy(tmp, names[i], sizeof(tmp));
        j = i - 1;
        while (j >= 0 && mtimes[j] > mt) {
            mtimes[j + 1] = mtimes[j];
            strncpy(names[j + 1], names[j], 256);
            --j;
        }
        mtimes[j + 1] = mt;
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
                }
                ++n;
            }
        }
        c.count = n;
    }

    /* 2. delete oldest beyond max_backup_files */
    if (fs->max_backup_files > 0 && c.count > fs->max_backup_files) {
        int to_delete = c.count - fs->max_backup_files;
        for (i = 0; i < to_delete; ++i) {
            char full[HX_CLOG_PATH_MAX];
            snprintf(full, sizeof(full), "%s/%s", fs->dir, names[i]);
            hx_remove(full);
        }
    }
}

/* Decide whether rotation is needed and perform it. Caller holds fs->lock. */
int hx_rotate_maybe(struct hx_file_sink_impl* fs, unsigned int incoming) {
    hx_timestamp_t ts;
    struct tm tmv;
    int need_size = 0;
    int need_time = 0;
    char stem[256], ext[64];
    char date_tag[16];
    char archive[HX_CLOG_PATH_MAX];
    char archive_full[HX_CLOG_PATH_MAX];
    index_ctx ic;
    int index;

    hx_now(&ts);
    hx_localtime(ts.sec, &tmv);

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
        }
    }

    if (!need_size && !need_time) {
        return HX_CLOG_OK;
    }

    /* Use the date the *current* file belongs to for the archive tag. */
    {
        struct tm cur;
        time_t approx;
        /* derive a date string from the file's recorded day if possible;
         * fall back to "now" rounded to the file's day. */
        cur = tmv;
        if (!need_time) {
            /* size rotation within the same day: tag = today */
        } else {
            /* day changed: archive belongs to the previous day */
            approx = ts.sec - 86400;
            hx_localtime(approx, &cur);
        }
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
    hx_rename(fs->active_path, archive_full);

    /* reopen fresh active file */
    if (hx_file_open_active(fs) != HX_CLOG_OK) {
        return HX_CLOG_ERR_OPEN_FILE_FAILED;
    }
    fs->current_size = 0;

    hx_core_add_rotated(1);
    hx_rotate_cleanup(fs);
    return HX_CLOG_OK;
}
