/*
 * hx_clog - sink layer.
 *
 * Provides the generic sink dispatch helpers plus the console and callback
 * sinks. The file sink lives in hx_clog_file.c.
 */
#include "hx_clog_internal.h"

#if (defined(HX_PLATFORM_POSIX) || defined(HX_PLATFORM_APPLE)) && defined(HX_CLOG_ENABLE_SYSLOG)
#  include <syslog.h>
#endif

#if defined(HX_PLATFORM_ANDROID)
#  include <android/log.h>
#endif

#if defined(HX_PLATFORM_APPLE)
#  include <os/log.h>
#endif

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

static void system_sink_emit(hx_clog_sink_t* sink, hx_clog_level_t level,
                             const char* data, unsigned int size);

/* ---- generic dispatch ---- */

void hx_sink_write(hx_clog_sink_t* s, hx_clog_level_t level,
                   const char* data, unsigned int size) {
    if (!s) {
        return;
    }
    if (level < s->min_level) {
        return;
    }
    switch (s->kind) {
        case HX_SINK_KIND_CONSOLE:
            hx_sink_console_emit(s, level, data, size);
            return;
        case HX_SINK_KIND_CALLBACK:
            hx_sink_callback_set_level(s, level);
            break;
        case HX_SINK_KIND_SYSLOG:
        case HX_SINK_KIND_EVENT_LOG:
        case HX_SINK_KIND_ANDROID_LOG:
        case HX_SINK_KIND_APPLE_LOG:
            system_sink_emit(s, level, data, size);
            return;
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
    sink->id = 0;
    sink->min_level = HX_CLOG_LEVEL_TRACE;
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
    sink->id = 0;
    sink->min_level = HX_CLOG_LEVEL_TRACE;
    return sink;
}

/* =========================================================================
 * Platform system sinks
 * ========================================================================= */

#if (defined(HX_PLATFORM_POSIX) || defined(HX_PLATFORM_APPLE)) && defined(HX_CLOG_ENABLE_SYSLOG)
typedef struct {
    char ident[128];
} syslog_impl;

static int syslog_write(hx_clog_sink_t* sink, const char* data, unsigned int size) {
    (void)sink;
    syslog(LOG_USER | LOG_INFO, "%.*s", (int)size, data);
    return HX_CLOG_OK;
}

static int syslog_flush(hx_clog_sink_t* sink) {
    (void)sink;
    return HX_CLOG_OK;
}

static void syslog_close(hx_clog_sink_t* sink) {
    if (!sink) return;
    hx_clog__free(sink->impl);
    hx_clog__free(sink);
    closelog();
}

static const hx_clog_sink_vtable_t k_syslog_vtable = {
    syslog_write, syslog_flush, syslog_close
};
#endif

hx_clog_sink_t* hx_sink_syslog_create(const char* ident) {
#if (defined(HX_PLATFORM_POSIX) || defined(HX_PLATFORM_APPLE)) && defined(HX_CLOG_ENABLE_SYSLOG)
    hx_clog_sink_t* sink = (hx_clog_sink_t*)hx_clog__malloc(sizeof(*sink));
    syslog_impl* si;
    if (!sink) {
        return NULL;
    }
    si = (syslog_impl*)hx_clog__malloc(sizeof(*si));
    if (!si) {
        hx_clog__free(sink);
        return NULL;
    }
    memset(si, 0, sizeof(*si));
    strncpy(si->ident, ident && ident[0] ? ident : "hx_clog",
            sizeof(si->ident) - 1);
    openlog(si->ident, LOG_PID | LOG_NDELAY, LOG_USER);
    sink->vtable = &k_syslog_vtable;
    sink->impl = si;
    sink->kind = HX_SINK_KIND_SYSLOG;
    sink->wants_color = 0;
    sink->is_file = 0;
    sink->id = 0;
    sink->min_level = HX_CLOG_LEVEL_TRACE;
    return sink;
#else
    (void)ident;
    return NULL;
#endif
}

#if defined(HX_PLATFORM_WINDOWS)
typedef struct {
    HANDLE handle;
    char source[128];
} event_log_impl;

static WORD event_type_from_level(hx_clog_level_t level) {
    if (level >= HX_CLOG_LEVEL_ERROR) return EVENTLOG_ERROR_TYPE;
    if (level >= HX_CLOG_LEVEL_WARN)  return EVENTLOG_WARNING_TYPE;
    return EVENTLOG_INFORMATION_TYPE;
}

static int event_log_write(hx_clog_sink_t* sink, const char* data, unsigned int size) {
    event_log_impl* ei = (event_log_impl*)sink->impl;
    char* msg;
    LPCSTR strings[1];
    if (!ei || !ei->handle) {
        return HX_CLOG_ERR_PLATFORM;
    }
    msg = (char*)hx_clog__malloc(size + 1);
    if (!msg) {
        return HX_CLOG_ERR_OUT_OF_MEMORY;
    }
    memcpy(msg, data, size);
    msg[size] = '\0';
    strings[0] = msg;
    ReportEventA(ei->handle,
                 event_type_from_level(sink->min_level),
                 0, 0, NULL, 1, 0, strings, NULL);
    hx_clog__free(msg);
    return HX_CLOG_OK;
}

static int event_log_flush(hx_clog_sink_t* sink) {
    (void)sink;
    return HX_CLOG_OK;
}

static void event_log_close(hx_clog_sink_t* sink) {
    event_log_impl* ei;
    if (!sink) return;
    ei = (event_log_impl*)sink->impl;
    if (ei && ei->handle) {
        DeregisterEventSource(ei->handle);
    }
    hx_clog__free(ei);
    hx_clog__free(sink);
}

static const hx_clog_sink_vtable_t k_event_log_vtable = {
    event_log_write, event_log_flush, event_log_close
};
#endif

hx_clog_sink_t* hx_sink_event_log_create(const char* source) {
#if defined(HX_PLATFORM_WINDOWS)
    hx_clog_sink_t* sink = (hx_clog_sink_t*)hx_clog__malloc(sizeof(*sink));
    event_log_impl* ei;
    if (!sink) {
        return NULL;
    }
    ei = (event_log_impl*)hx_clog__malloc(sizeof(*ei));
    if (!ei) {
        hx_clog__free(sink);
        return NULL;
    }
    memset(ei, 0, sizeof(*ei));
    strncpy(ei->source, source && source[0] ? source : "hx_clog",
            sizeof(ei->source) - 1);
    ei->handle = RegisterEventSourceA(NULL, ei->source);
    if (!ei->handle) {
        hx_clog__free(ei);
        hx_clog__free(sink);
        return NULL;
    }
    sink->vtable = &k_event_log_vtable;
    sink->impl = ei;
    sink->kind = HX_SINK_KIND_EVENT_LOG;
    sink->wants_color = 0;
    sink->is_file = 0;
    sink->id = 0;
    sink->min_level = HX_CLOG_LEVEL_TRACE;
    return sink;
#else
    (void)source;
    return NULL;
#endif
}

#if defined(HX_PLATFORM_ANDROID)
static int android_log_write(hx_clog_sink_t* sink, const char* data, unsigned int size) {
    char* tag = (char*)sink->impl;
    char* msg = (char*)hx_clog__malloc(size + 1);
    if (!msg) {
        return HX_CLOG_ERR_OUT_OF_MEMORY;
    }
    memcpy(msg, data, size);
    msg[size] = '\0';
    __android_log_write(ANDROID_LOG_INFO, tag ? tag : "hx_clog", msg);
    hx_clog__free(msg);
    return HX_CLOG_OK;
}

static int android_log_flush(hx_clog_sink_t* sink) {
    (void)sink;
    return HX_CLOG_OK;
}

static void android_log_close(hx_clog_sink_t* sink) {
    if (!sink) return;
    hx_clog__free(sink->impl);
    hx_clog__free(sink);
}

static const hx_clog_sink_vtable_t k_android_log_vtable = {
    android_log_write, android_log_flush, android_log_close
};
#endif

hx_clog_sink_t* hx_sink_android_log_create(const char* tag) {
#if defined(HX_PLATFORM_ANDROID)
    hx_clog_sink_t* sink = (hx_clog_sink_t*)hx_clog__malloc(sizeof(*sink));
    char* owned;
    if (!sink) {
        return NULL;
    }
    owned = (char*)hx_clog__malloc(128);
    if (!owned) {
        hx_clog__free(sink);
        return NULL;
    }
    strncpy(owned, tag && tag[0] ? tag : "hx_clog", 127);
    owned[127] = '\0';
    sink->vtable = &k_android_log_vtable;
    sink->impl = owned;
    sink->kind = HX_SINK_KIND_ANDROID_LOG;
    sink->wants_color = 0;
    sink->is_file = 0;
    sink->id = 0;
    sink->min_level = HX_CLOG_LEVEL_TRACE;
    return sink;
#else
    (void)tag;
    return NULL;
#endif
}

#if defined(HX_PLATFORM_APPLE)
typedef struct {
    os_log_t log;
    char subsystem[128];
} apple_log_impl;

static int apple_log_write(hx_clog_sink_t* sink, const char* data, unsigned int size) {
    apple_log_impl* ai = (apple_log_impl*)sink->impl;
    char* msg = (char*)hx_clog__malloc(size + 1);
    if (!msg) {
        return HX_CLOG_ERR_OUT_OF_MEMORY;
    }
    memcpy(msg, data, size);
    msg[size] = '\0';
    os_log_with_type(ai->log ? ai->log : OS_LOG_DEFAULT,
                     OS_LOG_TYPE_INFO, "%{public}s", msg);
    hx_clog__free(msg);
    return HX_CLOG_OK;
}

static int apple_log_flush(hx_clog_sink_t* sink) {
    (void)sink;
    return HX_CLOG_OK;
}

static void apple_log_close(hx_clog_sink_t* sink) {
    if (!sink) return;
    hx_clog__free(sink->impl);
    hx_clog__free(sink);
}

static const hx_clog_sink_vtable_t k_apple_log_vtable = {
    apple_log_write, apple_log_flush, apple_log_close
};
#endif

hx_clog_sink_t* hx_sink_apple_log_create(const char* subsystem) {
#if defined(HX_PLATFORM_APPLE)
    hx_clog_sink_t* sink = (hx_clog_sink_t*)hx_clog__malloc(sizeof(*sink));
    apple_log_impl* ai;
    if (!sink) {
        return NULL;
    }
    ai = (apple_log_impl*)hx_clog__malloc(sizeof(*ai));
    if (!ai) {
        hx_clog__free(sink);
        return NULL;
    }
    memset(ai, 0, sizeof(*ai));
    strncpy(ai->subsystem, subsystem && subsystem[0] ? subsystem : "hx_clog",
            sizeof(ai->subsystem) - 1);
    ai->log = os_log_create(ai->subsystem, "default");
    sink->vtable = &k_apple_log_vtable;
    sink->impl = ai;
    sink->kind = HX_SINK_KIND_APPLE_LOG;
    sink->wants_color = 0;
    sink->is_file = 0;
    sink->id = 0;
    sink->min_level = HX_CLOG_LEVEL_TRACE;
    return sink;
#else
    (void)subsystem;
    return NULL;
#endif
}

static void system_sink_emit(hx_clog_sink_t* sink, hx_clog_level_t level,
                             const char* data, unsigned int size) {
#if (defined(HX_PLATFORM_POSIX) || defined(HX_PLATFORM_APPLE)) && defined(HX_CLOG_ENABLE_SYSLOG)
    if (sink->kind == HX_SINK_KIND_SYSLOG) {
        int pri;
        switch (level) {
            case HX_CLOG_LEVEL_TRACE:
            case HX_CLOG_LEVEL_DEBUG: pri = LOG_DEBUG; break;
            case HX_CLOG_LEVEL_INFO:  pri = LOG_INFO; break;
            case HX_CLOG_LEVEL_WARN:  pri = LOG_WARNING; break;
            case HX_CLOG_LEVEL_ERROR: pri = LOG_ERR; break;
            case HX_CLOG_LEVEL_FATAL: pri = LOG_CRIT; break;
            default:                  pri = LOG_INFO; break;
        }
        syslog(LOG_USER | pri, "%.*s", (int)size, data);
        return;
    }
#endif
#if defined(HX_PLATFORM_APPLE)
    if (sink->kind == HX_SINK_KIND_APPLE_LOG) {
        apple_log_impl* ai = (apple_log_impl*)sink->impl;
        char* msg = (char*)hx_clog__malloc(size + 1);
        os_log_type_t type = OS_LOG_TYPE_INFO;
        if (!msg) {
            return;
        }
        switch (level) {
            case HX_CLOG_LEVEL_TRACE:
            case HX_CLOG_LEVEL_DEBUG: type = OS_LOG_TYPE_DEBUG; break;
            case HX_CLOG_LEVEL_INFO:  type = OS_LOG_TYPE_INFO; break;
            case HX_CLOG_LEVEL_WARN:  type = OS_LOG_TYPE_DEFAULT; break;
            case HX_CLOG_LEVEL_ERROR:
            case HX_CLOG_LEVEL_FATAL: type = OS_LOG_TYPE_ERROR; break;
            default:                  type = OS_LOG_TYPE_INFO; break;
        }
        memcpy(msg, data, size);
        msg[size] = '\0';
        os_log_with_type(ai && ai->log ? ai->log : OS_LOG_DEFAULT,
                         type, "%{public}s", msg);
        hx_clog__free(msg);
        return;
    }
#endif
#if defined(HX_PLATFORM_WINDOWS)
    if (sink->kind == HX_SINK_KIND_EVENT_LOG) {
        event_log_impl* ei = (event_log_impl*)sink->impl;
        char* msg;
        LPCSTR strings[1];
        if (!ei || !ei->handle) {
            return;
        }
        msg = (char*)hx_clog__malloc(size + 1);
        if (!msg) {
            return;
        }
        memcpy(msg, data, size);
        msg[size] = '\0';
        strings[0] = msg;
        ReportEventA(ei->handle, event_type_from_level(level),
                     0, 0, NULL, 1, 0, strings, NULL);
        hx_clog__free(msg);
        return;
    }
#endif
#if defined(HX_PLATFORM_ANDROID)
    if (sink->kind == HX_SINK_KIND_ANDROID_LOG) {
        char* tag = (char*)sink->impl;
        char* msg = (char*)hx_clog__malloc(size + 1);
        int prio = ANDROID_LOG_INFO;
        if (!msg) {
            return;
        }
        switch (level) {
            case HX_CLOG_LEVEL_TRACE: prio = ANDROID_LOG_VERBOSE; break;
            case HX_CLOG_LEVEL_DEBUG: prio = ANDROID_LOG_DEBUG; break;
            case HX_CLOG_LEVEL_INFO:  prio = ANDROID_LOG_INFO; break;
            case HX_CLOG_LEVEL_WARN:  prio = ANDROID_LOG_WARN; break;
            case HX_CLOG_LEVEL_ERROR: prio = ANDROID_LOG_ERROR; break;
            case HX_CLOG_LEVEL_FATAL: prio = ANDROID_LOG_FATAL; break;
            default:                  prio = ANDROID_LOG_INFO; break;
        }
        memcpy(msg, data, size);
        msg[size] = '\0';
        __android_log_write(prio, tag ? tag : "hx_clog", msg);
        hx_clog__free(msg);
        return;
    }
#endif
    if (sink->vtable && sink->vtable->write) {
        sink->vtable->write(sink, data, size);
    }
}
