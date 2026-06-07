/*
 * hx_clog - sink layer.
 *
 * Provides the generic sink dispatch helpers plus the console and callback
 * sinks. The file sink lives in hx_clog_file.c.
 */
#include "hx_clog_internal.h"

/* ANSI colors per level (used only when color is enabled and on a tty). */
static const char* level_color(hx_clog_level_t level) {
    switch (level) {
        case HX_CLOG_LEVEL_TRACE: return "\033[37m";   /* white   */
        case HX_CLOG_LEVEL_DEBUG: return "\033[36m";   /* cyan    */
        case HX_CLOG_LEVEL_INFO:  return "\033[32m";   /* green   */
        case HX_CLOG_LEVEL_WARN:  return "\033[33m";   /* yellow  */
        case HX_CLOG_LEVEL_ERROR: return "\033[31m";   /* red     */
        case HX_CLOG_LEVEL_FATAL: return "\033[1;41m"; /* red bg  */
        default:                  return "\033[0m";
    }
}
#define COLOR_RESET "\033[0m"

/* ---- generic dispatch ---- */

void hx_sink_write(hx_clog_sink_t* s, hx_clog_level_t level,
                   const char* data, unsigned int size) {
    if (!s) {
        return;
    }
    switch (s->kind) {
        case HX_SINK_KIND_CONSOLE:
            hx_sink_console_emit(s, level, data, size);
            return;
        case HX_SINK_KIND_CALLBACK:
            hx_sink_callback_set_level(s, level);
            break;
        case HX_SINK_KIND_FILE:
        default:
            break;
    }
    if (s->vtable && s->vtable->write) {
        s->vtable->write(s, data, size);
    }
}

void hx_sink_flush(hx_clog_sink_t* s) {
    if (s && s->vtable && s->vtable->flush) {
        s->vtable->flush(s);
    }
}

void hx_sink_close(hx_clog_sink_t* s) {
    if (s && s->vtable && s->vtable->close) {
        s->vtable->close(s);
    }
}

/* =========================================================================
 * Console sink
 *
 * The console sink needs the level to choose a color. We expose a dedicated
 * write that carries the level. Generic hx_sink_write can't pass it, so the
 * core calls hx_sink_console_write directly when the sink is a console sink.
 * To keep things uniform we instead encode color inline by having the core
 * format already; simplest robust approach: console sink stores the "current
 * level" set right before each write via hx_sink_console_set_level.
 * ========================================================================= */

typedef struct {
    FILE* out;             /* stdout */
    FILE* err;             /* stderr for WARN+ */
    int   enable_color;
    int   color_out_tty;
    int   color_err_tty;
} console_impl;

/* level is conveyed through a small thread-local-ish trick: store in impl
 * under the core's single-threaded-per-write contract is unsafe, so we
 * instead inspect the formatted text. Cleanest: the console write receives
 * level from the core via hx_core. We add an explicit entry point. */

static int is_tty(FILE* f) {
#if defined(HX_PLATFORM_WINDOWS)
    return _isatty(_fileno(f));
#else
    return isatty(fileno(f));
#endif
}

static int console_write(hx_clog_sink_t* sink, const char* data, unsigned int size) {
    console_impl* c = (console_impl*)sink->impl;
    FILE* out = c->out ? c->out : stdout;
    fwrite(data, 1, size, out);
    return HX_CLOG_OK;
}

static int console_flush(hx_clog_sink_t* sink) {
    console_impl* c = (console_impl*)sink->impl;
    if (c->out) fflush(c->out);
    if (c->err) fflush(c->err);
    return HX_CLOG_OK;
}

static void console_close(hx_clog_sink_t* sink) {
    if (!sink) return;
    hx_clog__free(sink->impl);
    hx_clog__free(sink);
}

static const hx_clog_sink_vtable_t k_console_vtable = {
    console_write, console_flush, console_close
};

/* Public-to-core entry that honors color and level routing. */
void hx_sink_console_emit(hx_clog_sink_t* sink, hx_clog_level_t level,
                          const char* data, unsigned int size) {
    console_impl* c = (console_impl*)sink->impl;
    FILE* target = (level >= HX_CLOG_LEVEL_WARN && c->err) ? c->err : c->out;
    int tty = (target == c->err) ? c->color_err_tty : c->color_out_tty;

    if (!target) {
        target = stdout;
    }
    if (c->enable_color && tty) {
        const char* col = level_color(level);
        fwrite(col, 1, strlen(col), target);
        fwrite(data, 1, size, target);
        fwrite(COLOR_RESET, 1, strlen(COLOR_RESET), target);
    } else {
        fwrite(data, 1, size, target);
    }
}

hx_clog_sink_t* hx_sink_console_create(int use_stderr_for_errors, int enable_color) {
    hx_clog_sink_t* sink = (hx_clog_sink_t*)hx_clog__malloc(sizeof(*sink));
    console_impl* c;
    if (!sink) {
        return NULL;
    }
    c = (console_impl*)hx_clog__malloc(sizeof(*c));
    if (!c) {
        hx_clog__free(sink);
        return NULL;
    }
    memset(c, 0, sizeof(*c));
    c->out = stdout;
    c->err = use_stderr_for_errors ? stderr : stdout;
    c->enable_color = enable_color;
    c->color_out_tty = is_tty(stdout);
    c->color_err_tty = is_tty(stderr);

    sink->vtable = &k_console_vtable;
    sink->impl = c;
    sink->kind = HX_SINK_KIND_CONSOLE;
    sink->wants_color = enable_color;
    sink->is_file = 0;
    return sink;
}

/* =========================================================================
 * Callback sink
 * ========================================================================= */

typedef struct {
    hx_clog_callback_t cb;
    void* user_data;
    hx_clog_level_t last_level;
} callback_impl;

static int callback_write(hx_clog_sink_t* sink, const char* data, unsigned int size) {
    callback_impl* ci = (callback_impl*)sink->impl;
    if (ci->cb) {
        return ci->cb(ci->last_level, data, size, ci->user_data);
    }
    return HX_CLOG_OK;
}

static int callback_flush(hx_clog_sink_t* sink) {
    (void)sink;
    return HX_CLOG_OK;
}

static void callback_close(hx_clog_sink_t* sink) {
    if (!sink) return;
    hx_clog__free(sink->impl);
    hx_clog__free(sink);
}

static const hx_clog_sink_vtable_t k_callback_vtable = {
    callback_write, callback_flush, callback_close
};

void hx_sink_callback_set_level(hx_clog_sink_t* sink, hx_clog_level_t level) {
    callback_impl* ci = (callback_impl*)sink->impl;
    ci->last_level = level;
}

hx_clog_sink_t* hx_sink_callback_create(hx_clog_callback_t cb, void* user_data) {
    hx_clog_sink_t* sink = (hx_clog_sink_t*)hx_clog__malloc(sizeof(*sink));
    callback_impl* ci;
    if (!sink) {
        return NULL;
    }
    ci = (callback_impl*)hx_clog__malloc(sizeof(*ci));
    if (!ci) {
        hx_clog__free(sink);
        return NULL;
    }
    ci->cb = cb;
    ci->user_data = user_data;
    ci->last_level = HX_CLOG_LEVEL_INFO;

    sink->vtable = &k_callback_vtable;
    sink->impl = ci;
    sink->kind = HX_SINK_KIND_CALLBACK;
    sink->wants_color = 0;
    sink->is_file = 0;
    return sink;
}
