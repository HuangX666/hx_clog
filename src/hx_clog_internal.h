/*
 * hx_clog - internal shared declarations.
 *
 * Not installed. Used to share state and helpers between translation units.
 */
#ifndef HX_CLOG_INTERNAL_H
#define HX_CLOG_INTERNAL_H

/* MinGW's default printf family routes to the old msvcrt, whose vsnprintf does
 * NOT follow C99: on truncation it returns -1 instead of the required length.
 * The write path relies on that length to size the heap retry, so without this
 * a long message makes vsnprintf return -1 and the whole line is dropped. Ask
 * MinGW for its C99-conformant implementation. Must be defined before any
 * system header (stdarg.h below pulls in _mingw.h, which latches this). */
#if defined(__MINGW32__) && !defined(__USE_MINGW_ANSI_STDIO)
#  define __USE_MINGW_ANSI_STDIO 1
#endif

#include "hx_clog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Platform detection
 * ------------------------------------------------------------------------- */
#if defined(_WIN32)
#  define HX_PLATFORM_WINDOWS 1
#elif defined(__APPLE__)
#  define HX_PLATFORM_APPLE 1
#  define HX_PLATFORM_POSIX 1
#elif defined(__ANDROID__)
#  define HX_PLATFORM_ANDROID 1
#  define HX_PLATFORM_POSIX 1
#elif defined(__unix__) || defined(__linux__)
#  define HX_PLATFORM_UNIX 1
#  define HX_PLATFORM_POSIX 1
#endif

#if defined(HX_PLATFORM_WINDOWS)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <process.h>
#  include <io.h>
#  include <direct.h>
#else
#  include <pthread.h>
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/time.h>
#  include <sys/types.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Tunables
 * ------------------------------------------------------------------------- */
#define HX_CLOG_STACK_BUF_SIZE   1024   /* stack fast-path buffer */

/*
 * Maximum size of a single formatted log line.
 *
 * Default cap is 512 KB and applies to BOTH sync and async modes. Define
 * HX_CLOG_UNLIMITED_LINE (CMake option HX_CLOG_UNLIMITED_LINE=ON) to remove the
 * cap entirely: a line may then grow to whatever the process can allocate. The
 * cap can also be overridden at build time via -DHX_CLOG_MAX_LINE=<bytes>.
 */
#if !defined(HX_CLOG_MAX_LINE)
#  define HX_CLOG_MAX_LINE       (512 * 1024)
#endif

#define HX_CLOG_MAX_SINKS        16
#define HX_CLOG_PATH_MAX         1024
#define HX_CLOG_RING_CAPACITY    1024   /* crash ring buffer entries */
#define HX_CLOG_RING_ENTRY_SIZE  512    /* bytes per crash ring entry */

/* Clamp a requested byte count to the configured line cap (no-op when
 * HX_CLOG_UNLIMITED_LINE is defined). */
static inline unsigned int hx_clamp_line(unsigned int need) {
#if defined(HX_CLOG_UNLIMITED_LINE)
    return need;
#else
    return need > (unsigned int)HX_CLOG_MAX_LINE ? (unsigned int)HX_CLOG_MAX_LINE : need;
#endif
}

/* -------------------------------------------------------------------------
 * Allocator helpers (route through user allocator if set)
 * ------------------------------------------------------------------------- */
void* hx_clog__malloc(size_t size);
void* hx_clog__realloc(void* ptr, size_t old_size, size_t new_size);
void  hx_clog__free(void* ptr);
void  hx_clog__set_allocator(const hx_clog_allocator_t* allocator);

/* -------------------------------------------------------------------------
 * Threading primitives
 * ------------------------------------------------------------------------- */
#if defined(HX_PLATFORM_WINDOWS)
typedef CRITICAL_SECTION hx_mutex_t;
typedef CONDITION_VARIABLE hx_cond_t;
typedef HANDLE hx_thread_t;
#else
typedef pthread_mutex_t hx_mutex_t;
typedef pthread_cond_t  hx_cond_t;
typedef pthread_t       hx_thread_t;
#endif

void hx_mutex_init(hx_mutex_t* m);
/* Like hx_mutex_init but the mutex may be re-locked by the thread that already
 * holds it. Used for sink_lock so a user callback running under the lock can
 * safely re-enter the public API (e.g. get_sink_count) without self-deadlock.
 * Windows CRITICAL_SECTION is already recursive; POSIX needs an explicit attr. */
void hx_mutex_init_recursive(hx_mutex_t* m);
void hx_mutex_destroy(hx_mutex_t* m);
void hx_mutex_lock(hx_mutex_t* m);
void hx_mutex_unlock(hx_mutex_t* m);

void hx_cond_init(hx_cond_t* c);
void hx_cond_destroy(hx_cond_t* c);
void hx_cond_signal(hx_cond_t* c);
void hx_cond_broadcast(hx_cond_t* c);
/* Wait with timeout in milliseconds. Returns 1 if signalled, 0 if timed out. */
int  hx_cond_wait_ms(hx_cond_t* c, hx_mutex_t* m, unsigned int timeout_ms);
void hx_cond_wait(hx_cond_t* c, hx_mutex_t* m);

typedef void (*hx_thread_fn)(void* arg);
int  hx_thread_create(hx_thread_t* t, hx_thread_fn fn, void* arg);
void hx_thread_join(hx_thread_t t);

/* -------------------------------------------------------------------------
 * Atomic level (relaxed semantics are fine for filtering)
 * ------------------------------------------------------------------------- */
void            hx_atomic_store_level(volatile int* p, int v);
int             hx_atomic_load_level(volatile int* p);

/* -------------------------------------------------------------------------
 * Platform helpers
 * ------------------------------------------------------------------------- */
unsigned long hx_get_pid(void);
unsigned long hx_get_tid(void);
void          hx_sleep_ms(unsigned int ms);

/* Time with millisecond precision. */
typedef struct hx_timestamp {
    time_t       sec;
    unsigned int msec;
} hx_timestamp_t;

void hx_now(hx_timestamp_t* ts);
void hx_localtime(time_t t, struct tm* out);

/* Filesystem.
 *
 * All paths are UTF-8. On Windows they are converted to UTF-16 internally so
 * non-ASCII directories work regardless of the system ANSI codepage. */
int  hx_mkdir_p(const char* path);
int  hx_file_exists(const char* path);
long long hx_file_size(const char* path);
int  hx_rename(const char* from, const char* to);
int  hx_remove(const char* path);
FILE* hx_fopen(const char* path, const char* mode);

#if defined(HX_PLATFORM_WINDOWS)
/* UTF-8 <-> UTF-16 helpers (NUL-terminated). Return wide/byte length
 * including the terminator, or -1 on failure / overflow. */
int hx_utf8_to_wide(const char* utf8, wchar_t* out, int out_cap);
int hx_wide_to_utf8(const wchar_t* wide, char* out, int out_cap);
#endif

/* -------------------------------------------------------------------------
 * Sink abstraction (declared in public header as opaque)
 * ------------------------------------------------------------------------- */
typedef struct hx_clog_sink hx_clog_sink_t;

typedef struct hx_clog_sink_vtable {
    int  (*write)(hx_clog_sink_t* sink, const char* data, unsigned int size);
    int  (*flush)(hx_clog_sink_t* sink);
    void (*close)(hx_clog_sink_t* sink);
} hx_clog_sink_vtable_t;

typedef enum hx_sink_kind {
    HX_SINK_KIND_CONSOLE = 0,
    HX_SINK_KIND_FILE,
    HX_SINK_KIND_CALLBACK,
    HX_SINK_KIND_SYSLOG,
    HX_SINK_KIND_EVENT_LOG,
    HX_SINK_KIND_ANDROID_LOG,
    HX_SINK_KIND_APPLE_LOG
} hx_sink_kind_t;

#define HX_SINK_PATTERN_MAX 256

struct hx_clog_sink {
    const hx_clog_sink_vtable_t* vtable;
    void* impl;            /* sink-specific state */
    hx_sink_kind_t kind;
    int   wants_color;     /* console-only flag */
    int   is_file;         /* used by reopen/rotate */
    hx_clog_sink_id_t id;
    hx_clog_level_t min_level;

    /* per-sink format override (guarded by the core sink lock) */
    int   override_set;                       /* 0 = use global format */
    hx_clog_format_mode_t override_mode;
    int   override_has_pattern;
    char  override_pattern[HX_SINK_PATTERN_MAX];
};

/* console / callback level-aware helpers (sink layer) */
void hx_sink_console_emit(hx_clog_sink_t* sink, hx_clog_level_t level,
                          const char* data, unsigned int size);
void hx_sink_callback_set_level(hx_clog_sink_t* sink, hx_clog_level_t level);

/* Built-in sink factories. */
hx_clog_sink_t* hx_sink_console_create(int use_stderr_for_errors, int enable_color);
hx_clog_sink_t* hx_sink_file_create(const char* dir, const char* file_name,
                                    const hx_clog_config_t* cfg);
hx_clog_sink_t* hx_sink_callback_create(hx_clog_callback_t cb, void* user_data);
hx_clog_sink_t* hx_sink_syslog_create(const char* ident);
hx_clog_sink_t* hx_sink_event_log_create(const char* source);
hx_clog_sink_t* hx_sink_android_log_create(const char* tag);
hx_clog_sink_t* hx_sink_apple_log_create(const char* subsystem);

void hx_sink_write(hx_clog_sink_t* s, hx_clog_level_t level,
                   const char* data, unsigned int size);
void hx_sink_flush(hx_clog_sink_t* s);
void hx_sink_close(hx_clog_sink_t* s);

/* File sink specific operations (used by core for reopen). */
int  hx_sink_file_reopen(hx_clog_sink_t* s);

/* -------------------------------------------------------------------------
 * Formatting
 * ------------------------------------------------------------------------- */
/* Format a record into out (NUL terminated). Returns number of bytes written
 * (excluding NUL), capped at out_size-1. */
unsigned int hx_format_record(const char* pattern,
                              const hx_clog_record_t* rec,
                              char* out, unsigned int out_size);
unsigned int hx_format_json_record(const hx_clog_record_t* rec,
                                   char* out, unsigned int out_size);

const char* hx_level_short_name(hx_clog_level_t level); /* "INFO ", padded */

/* Thread-local context helpers. */
void hx_context_snapshot_text(char* out, unsigned int cap);

/* -------------------------------------------------------------------------
 * Crash ring buffer (the "last N logs")
 * ------------------------------------------------------------------------- */
void hx_ring_init(void);
void hx_ring_push(const char* line, unsigned int len);
/* Dump ring buffer to an fd/FILE using only simple writes. */
void hx_ring_dump_fd(int fd);
void hx_ring_destroy(void);

/* -------------------------------------------------------------------------
 * Rotation (called from file sink)
 * ------------------------------------------------------------------------- */
struct hx_file_sink_impl;
int  hx_rotate_maybe(struct hx_file_sink_impl* fs, unsigned int incoming);
int  hx_rotate_force(struct hx_file_sink_impl* fs);
void hx_rotate_cleanup(struct hx_file_sink_impl* fs);

/* -------------------------------------------------------------------------
 * Async engine
 * ------------------------------------------------------------------------- */
#if defined(HX_CLOG_ENABLE_ASYNC)
int  hx_async_start(const hx_clog_config_t* cfg);
void hx_async_stop(void);            /* drains and joins worker */
/* target_sink_id: 0 = every sink without a format override; otherwise only
 * the sink with that id. count_stats: increment written_lines when emitted. */
int  hx_async_enqueue(hx_clog_level_t level, const char* data, unsigned int size,
                      hx_clog_sink_id_t target_sink_id, int count_stats);
void hx_async_flush(void);
void hx_async_after_fork_child(void); /* re-init locks, restart worker */
unsigned long long hx_async_dropped(void);
unsigned long long hx_async_high_watermark(void);
/* Acquire/release the async queue lock; used only by the pthread_atfork
 * handlers so the queue lock is held across fork() and inherited unlocked. */
void hx_async_atfork_lock(void);
void hx_async_atfork_unlock(void);
#endif

/* -------------------------------------------------------------------------
 * Core internal callbacks used by the async worker / sinks.
 * ------------------------------------------------------------------------- */
/* Write a fully-formatted line to the matching sinks (synchronous path).
 * target_sink_id: 0 = every sink without a format override; otherwise only
 * the sink with that id. count_stats: increment written_lines once. */
void hx_core_emit_to_sinks(hx_clog_level_t level, const char* line,
                           unsigned int len, hx_clog_sink_id_t target_sink_id,
                           int count_stats);
void hx_core_flush_sinks(void);

/* Stats accessors. */
void hx_core_add_written(unsigned long long n);
void hx_core_add_rotated(unsigned long long n);

/* Report an internal failure to the user-installed error handler (no-op when
 * none is installed). Never call while holding the handler's own lock. */
void hx_core_report_error(int err, const char* message);

/* Crash callback accessor (storage lives in core so the setter works even
 * when crash support is compiled out). */
hx_clog_crash_callback_t hx_crash_get_callback(void** user_data_out);

/* File sink fork helper: re-init the sink's internal lock in the child. */
void hx_sink_file_after_fork(hx_clog_sink_t* s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HX_CLOG_INTERNAL_H */
