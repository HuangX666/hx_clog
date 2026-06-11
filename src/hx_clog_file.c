/*
 * hx_clog - file sink.
 *
 * Buffered file output with optional rotation. The sink owns a FILE* and a
 * mutex; rotation logic lives in hx_clog_rotate.c but operates on this
 * struct.
 */
#include "hx_clog_file.h"

static void join_path(char* out, unsigned int cap,
                      const char* dir, const char* name) {
    unsigned int n = 0;
    const char* p;
    if (dir && dir[0]) {
        for (p = dir; *p && n + 1 < cap; ++p) {
            out[n++] = *p;
        }
        if (n > 0 && out[n - 1] != '/' && out[n - 1] != '\\' && n + 1 < cap) {
            out[n++] = '/';
        }
    }
    for (p = name; p && *p && n + 1 < cap; ++p) {
        out[n++] = *p;
    }
    out[n] = '\0';
}

int hx_file_open_active(struct hx_file_sink_impl* fs) {
    hx_timestamp_t ts;
    struct tm tmv;
    long long sz;

    join_path(fs->active_path, sizeof(fs->active_path), fs->dir, fs->base_name);

    fs->fp = fopen(fs->active_path, "ab");
    if (!fs->fp) {
        return HX_CLOG_ERR_OPEN_FILE_FAILED;
    }

    sz = hx_file_size(fs->active_path);
    fs->current_size = (sz > 0) ? (unsigned long long)sz : 0ULL;

    hx_now(&ts);
    hx_localtime(ts.sec, &tmv);
    fs->cur_year = tmv.tm_year;
    fs->cur_yday = tmv.tm_yday;
    fs->opened_sec = ts.sec;
    return HX_CLOG_OK;
}

/* ---- sink vtable ---- */

static int file_write(hx_clog_sink_t* sink, const char* data, unsigned int size) {
    struct hx_file_sink_impl* fs = (struct hx_file_sink_impl*)sink->impl;
    size_t written;

    hx_mutex_lock(&fs->lock);

    if (fs->policy != HX_CLOG_ROTATE_NONE || fs->rotate_daily) {
        hx_rotate_maybe(fs, size);
    }

    if (!fs->fp) {
        hx_mutex_unlock(&fs->lock);
        return HX_CLOG_ERR_OPEN_FILE_FAILED;
    }

    written = fwrite(data, 1, size, fs->fp);
    fs->current_size += (unsigned long long)written;

    hx_mutex_unlock(&fs->lock);
    return (written == size) ? HX_CLOG_OK : HX_CLOG_ERR_PLATFORM;
}

static int file_flush(hx_clog_sink_t* sink) {
    struct hx_file_sink_impl* fs = (struct hx_file_sink_impl*)sink->impl;
    int r = 0;
    hx_mutex_lock(&fs->lock);
    if (fs->fp) {
        r = fflush(fs->fp);
    }
    hx_mutex_unlock(&fs->lock);
    return r == 0 ? HX_CLOG_OK : HX_CLOG_ERR_PLATFORM;
}

static void file_close(hx_clog_sink_t* sink) {
    struct hx_file_sink_impl* fs = (struct hx_file_sink_impl*)sink->impl;
    if (!fs) {
        return;
    }
    hx_mutex_lock(&fs->lock);
    if (fs->fp) {
        fflush(fs->fp);
        fclose(fs->fp);
        fs->fp = NULL;
    }
    hx_mutex_unlock(&fs->lock);
    hx_mutex_destroy(&fs->lock);
    hx_clog__free(fs);
}

static const hx_clog_sink_vtable_t k_file_vtable = {
    file_write, file_flush, file_close
};

hx_clog_sink_t* hx_sink_file_create(const char* dir, const char* file_name,
                                    const hx_clog_config_t* cfg) {
    hx_clog_sink_t* sink;
    struct hx_file_sink_impl* fs;

    if (!file_name) {
        file_name = "app.log";
    }
    if (!dir) {
        dir = "./logs";
    }

    if (hx_mkdir_p(dir) != 0 && !hx_file_exists(dir)) {
        return NULL;
    }

    sink = (hx_clog_sink_t*)hx_clog__malloc(sizeof(*sink));
    if (!sink) {
        return NULL;
    }
    fs = (struct hx_file_sink_impl*)hx_clog__malloc(sizeof(*fs));
    if (!fs) {
        hx_clog__free(sink);
        return NULL;
    }
    memset(fs, 0, sizeof(*fs));

    strncpy(fs->dir, dir, sizeof(fs->dir) - 1);
    strncpy(fs->base_name, file_name, sizeof(fs->base_name) - 1);
    fs->policy = cfg ? cfg->rotate_policy : HX_CLOG_ROTATE_NONE;
    fs->max_file_size = cfg ? cfg->max_file_size : 0;
    fs->max_backup_files = cfg ? cfg->max_backup_files : 0;
    fs->max_backup_days = cfg ? cfg->max_backup_days : 0;
    fs->rotate_daily = cfg ? cfg->rotate_daily : 0;
    fs->rotate_interval_seconds = cfg ? cfg->rotate_interval_seconds : 0;
    fs->rotate_on_startup = cfg ? cfg->rotate_on_startup : 0;

    hx_mutex_init(&fs->lock);

    if (hx_file_open_active(fs) != HX_CLOG_OK) {
        hx_mutex_destroy(&fs->lock);
        hx_clog__free(fs);
        hx_clog__free(sink);
        return NULL;
    }
    if (fs->rotate_on_startup && fs->current_size > 0) {
        hx_rotate_force(fs);
    }

    sink->vtable = &k_file_vtable;
    sink->impl = fs;
    sink->kind = HX_SINK_KIND_FILE;
    sink->wants_color = 0;
    sink->is_file = 1;
    sink->id = 0;
    sink->min_level = HX_CLOG_LEVEL_TRACE;
    return sink;
}

int hx_sink_file_reopen(hx_clog_sink_t* s) {
    struct hx_file_sink_impl* fs;
    int r;
    if (!s || !s->is_file) {
        return HX_CLOG_ERR_INVALID_ARGUMENT;
    }
    fs = (struct hx_file_sink_impl*)s->impl;
    hx_mutex_lock(&fs->lock);
    if (fs->fp) {
        fflush(fs->fp);
        fclose(fs->fp);
        fs->fp = NULL;
    }
    r = hx_file_open_active(fs);
    hx_mutex_unlock(&fs->lock);
    return r;
}
