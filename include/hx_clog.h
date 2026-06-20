/*
 * hx_clog - A portable C/C++11 logging framework.
 *
 * Public C API. This single header is enough to use the basic logging
 * capabilities. The implementation lives in the src/ directory and is built
 * as a static or shared library via CMake.
 *
 * The API is intentionally C ABI only so it can be consumed easily from C,
 * C++, Rust, Go or any language with a C FFI.
 */
#ifndef HX_CLOG_H
#define HX_CLOG_H

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Export / visibility macro
 * ------------------------------------------------------------------------- */
#if defined(_WIN32)
#  if defined(HX_CLOG_BUILD_SHARED)
#    if defined(HX_CLOG_EXPORTS)
#      define HX_CLOG_API __declspec(dllexport)
#    else
#      define HX_CLOG_API __declspec(dllimport)
#    endif
#  else
#    define HX_CLOG_API
#  endif
#else
#  if defined(HX_CLOG_BUILD_SHARED)
#    define HX_CLOG_API __attribute__((visibility("default")))
#  else
#    define HX_CLOG_API
#  endif
#endif

/* -------------------------------------------------------------------------
 * Version
 * ------------------------------------------------------------------------- */
#define HX_CLOG_VERSION_MAJOR 1
#define HX_CLOG_VERSION_MINOR 2
#define HX_CLOG_VERSION_PATCH 0

/* -------------------------------------------------------------------------
 * Basic enums
 * ------------------------------------------------------------------------- */
typedef enum hx_clog_level {
    HX_CLOG_LEVEL_TRACE = 0,
    HX_CLOG_LEVEL_DEBUG,
    HX_CLOG_LEVEL_INFO,
    HX_CLOG_LEVEL_WARN,
    HX_CLOG_LEVEL_ERROR,
    HX_CLOG_LEVEL_FATAL,
    HX_CLOG_LEVEL_OFF
} hx_clog_level_t;

typedef enum hx_clog_mode {
    HX_CLOG_MODE_SYNC = 0,
    HX_CLOG_MODE_ASYNC
} hx_clog_mode_t;

typedef enum hx_clog_format_mode {
    HX_CLOG_FORMAT_PATTERN = 0,
    HX_CLOG_FORMAT_JSON
} hx_clog_format_mode_t;

typedef enum hx_clog_rotate_policy {
    HX_CLOG_ROTATE_NONE = 0,
    HX_CLOG_ROTATE_BY_SIZE,
    HX_CLOG_ROTATE_BY_TIME,
    HX_CLOG_ROTATE_BY_SIZE_AND_TIME
} hx_clog_rotate_policy_t;

/* Behaviour when the async queue is full. */
typedef enum hx_clog_overflow_policy {
    HX_CLOG_OVERFLOW_BLOCK = 0,  /* block caller until there is space */
    HX_CLOG_OVERFLOW_DROP_NEW,   /* drop the incoming log */
    HX_CLOG_OVERFLOW_DROP_OLD    /* drop the oldest queued log */
} hx_clog_overflow_policy_t;

/* -------------------------------------------------------------------------
 * Result / error codes
 * ------------------------------------------------------------------------- */
typedef enum hx_clog_result {
    HX_CLOG_OK = 0,
    HX_CLOG_ERR_INVALID_ARGUMENT = -1,
    HX_CLOG_ERR_NOT_INITIALIZED = -2,
    HX_CLOG_ERR_ALREADY_INITIALIZED = -3,
    HX_CLOG_ERR_OPEN_FILE_FAILED = -4,
    HX_CLOG_ERR_OUT_OF_MEMORY = -5,
    HX_CLOG_ERR_THREAD_FAILED = -6,
    HX_CLOG_ERR_QUEUE_FULL = -7,
    HX_CLOG_ERR_PLATFORM = -8
} hx_clog_result_t;

HX_CLOG_API const char* hx_clog_strerror(int err);

/* -------------------------------------------------------------------------
 * Public record / formatter types
 * ------------------------------------------------------------------------- */
typedef struct hx_clog_record {
    hx_clog_level_t level;
    const char* logger_name;
    const char* file;
    int line;
    const char* func;
    unsigned long pid;
    unsigned long tid;
    long long timestamp_sec;
    unsigned int timestamp_msec;
    const char* message;
    unsigned int message_len;
    const char* context;          /* text form: key=value key2=value2 */
} hx_clog_record_t;

typedef unsigned int (*hx_clog_formatter_t)(
    const hx_clog_record_t* record,
    char* out,
    unsigned int out_size,
    void* user_data);

typedef unsigned int hx_clog_sink_id_t;

typedef struct hx_clog_logger hx_clog_logger_t;

/* -------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */
typedef struct hx_clog_config {
    const char* logger_name;       /* default "hx_clog" */
    const char* log_dir;           /* default "./logs" */
    const char* file_name;         /* default "app.log" */
    hx_clog_level_t level;         /* default INFO */
    hx_clog_mode_t mode;           /* default SYNC */

    int enable_console;            /* default 1 */
    int enable_file;               /* default 1 */
    int enable_color;              /* default 1, console only */
    int enable_crash_handler;      /* default 0, opt-in */
    int enable_syslog;             /* Unix / Apple syslog */
    int enable_event_log;          /* Windows Event Log */
    int enable_android_log;        /* Android logcat */
    int enable_apple_log;          /* Apple os_log */

    hx_clog_rotate_policy_t rotate_policy;
    unsigned long long max_file_size;  /* e.g. 10 * 1024 * 1024 */
    int max_backup_files;              /* number of newest backups kept as plain
                                        * (uncompressed); older ones are
                                        * compressed. -1 = keep all as plain
                                        * (never compress, never delete);
                                        * 0 = never compress. Default -1. */
    int max_backup_days;               /* keep files for at most N days, 0=off */
    int rotate_daily;                  /* split by day */
    unsigned int rotate_interval_seconds; /* interval time rotation, 0=off */
    int rotate_on_startup;             /* archive an existing active log at init */

    unsigned int async_queue_size;     /* default 8192 */
    unsigned int async_batch_size;     /* default 64 */
    unsigned int flush_interval_ms;    /* default 1000 */
    hx_clog_overflow_policy_t overflow_policy; /* default BLOCK */

    const char* pattern;               /* default built-in pattern */
    hx_clog_format_mode_t format_mode; /* default PATTERN */
    hx_clog_formatter_t formatter;     /* optional custom formatter */
    void* formatter_user_data;
    const char* system_logger_name;    /* ident/source/tag for system sinks */

    /* -- added in 1.1.0 (always call hx_clog_config_default() before filling
     *    the struct) -- */
    int rotate_align;                  /* align interval rotation to wall-clock
                                        * boundaries (e.g. 3600 -> on the hour) */
    int max_compressed_files;          /* cap on .gz backups kept after
                                        * compression. -1 = never delete by
                                        * count (default); 0 = use
                                        * max_backup_files; >0 = that many. */

    /* -- added in 1.2.0 -- */
    int date_subdir;                   /* when 1, the active log is written under
                                        * a per-day folder: <log_dir>/<YYYY-MM-DD>/
                                        * <file_name>, created automatically and
                                        * switched at the day boundary. Default 0. */
} hx_clog_config_t;

HX_CLOG_API void hx_clog_config_default(hx_clog_config_t* config);

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */
HX_CLOG_API int  hx_clog_init(const hx_clog_config_t* config);
/* Initialize from an INI file (a `[hx_clog]` section of key=value lines; keys
 * mirror the config fields, e.g. level=debug, dir=./logs, max_file_size=10M,
 * mode=async). Missing keys keep their defaults. '#' and ';' start comments. */
HX_CLOG_API int  hx_clog_init_from_file(const char* path);
HX_CLOG_API void hx_clog_shutdown(void);
HX_CLOG_API void hx_clog_flush(void);
HX_CLOG_API int  hx_clog_is_initialized(void);
HX_CLOG_API int  hx_clog_reconfigure(const hx_clog_config_t* config);

/* Read back the currently-applied configuration (reflects env overrides and any
 * async->sync fallback). Returns HX_CLOG_ERR_NOT_INITIALIZED before init. The
 * string fields point at internal storage valid until the next
 * init/reconfigure — copy them if you need to keep them. */
HX_CLOG_API int  hx_clog_get_config(hx_clog_config_t* out_config);

HX_CLOG_API void            hx_clog_set_level(hx_clog_level_t level);
HX_CLOG_API hx_clog_level_t hx_clog_get_level(void);
HX_CLOG_API int             hx_clog_set_pattern(const char* pattern);
HX_CLOG_API int             hx_clog_set_format_mode(hx_clog_format_mode_t mode);
HX_CLOG_API int             hx_clog_set_formatter(hx_clog_formatter_t formatter,
                                                  void* user_data);

/* Reopen the log file (after logrotate or external move). */
HX_CLOG_API int hx_clog_reopen(void);

/* Convert a level to / from its short text name ("INFO", ...). */
HX_CLOG_API const char*     hx_clog_level_name(hx_clog_level_t level);
HX_CLOG_API hx_clog_level_t hx_clog_level_from_name(const char* name);

/* -------------------------------------------------------------------------
 * Writing logs
 * ------------------------------------------------------------------------- */
HX_CLOG_API void hx_clog_write(
    hx_clog_level_t level,
    const char* file,
    int line,
    const char* func,
    const char* fmt,
    ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 5, 6)))
#endif
    ;

HX_CLOG_API void hx_clog_writev(
    hx_clog_level_t level,
    const char* file,
    int line,
    const char* func,
    const char* fmt,
    va_list args);

HX_CLOG_API void hx_clog_write_named(
    hx_clog_level_t level,
    const char* logger_name,
    const char* file,
    int line,
    const char* func,
    const char* fmt,
    ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 6, 7)))
#endif
    ;

HX_CLOG_API void hx_clog_writev_named(
    hx_clog_level_t level,
    const char* logger_name,
    const char* file,
    int line,
    const char* func,
    const char* fmt,
    va_list args);

/* -------------------------------------------------------------------------
 * Named loggers / categories
 * ------------------------------------------------------------------------- */
HX_CLOG_API hx_clog_logger_t* hx_clog_default_logger(void);
HX_CLOG_API int  hx_clog_logger_create(const char* name,
                                       hx_clog_level_t level,
                                       hx_clog_logger_t** out_logger);
HX_CLOG_API void hx_clog_logger_destroy(hx_clog_logger_t* logger);
HX_CLOG_API int  hx_clog_logger_set_level(hx_clog_logger_t* logger,
                                          hx_clog_level_t level);
HX_CLOG_API hx_clog_level_t hx_clog_logger_get_level(
    const hx_clog_logger_t* logger);
HX_CLOG_API const char* hx_clog_logger_name(const hx_clog_logger_t* logger);

/* Registry + dotted-name hierarchy. hx_clog_logger_get returns the existing
 * logger of that name or creates+registers one (never lost — no pointer to
 * track). A new "a.b.c" inherits the level of its nearest registered ancestor
 * ("a.b", then "a"), else the global level. hx_clog_logger_find returns NULL if
 * absent. hx_clog_set_level_for_prefix sets the prefix logger and all current
 * descendants ("net" affects "net", "net.tcp", ...) and creates the prefix
 * logger so later descendants inherit too. Registry loggers live until
 * hx_clog_logger_drop_all() or process exit; do not pass them to
 * hx_clog_logger_destroy(). */
HX_CLOG_API hx_clog_logger_t* hx_clog_logger_get(const char* name);
HX_CLOG_API hx_clog_logger_t* hx_clog_logger_find(const char* name);
HX_CLOG_API unsigned int      hx_clog_logger_count(void);
HX_CLOG_API int  hx_clog_set_level_for_prefix(const char* prefix,
                                              hx_clog_level_t level);
HX_CLOG_API void hx_clog_logger_drop_all(void);
HX_CLOG_API void hx_clog_logger_write(
    hx_clog_logger_t* logger,
    hx_clog_level_t level,
    const char* file,
    int line,
    const char* func,
    const char* fmt,
    ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 6, 7)))
#endif
    ;
HX_CLOG_API void hx_clog_logger_writev(
    hx_clog_logger_t* logger,
    hx_clog_level_t level,
    const char* file,
    int line,
    const char* func,
    const char* fmt,
    va_list args);

/* -------------------------------------------------------------------------
 * Thread-local context
 * ------------------------------------------------------------------------- */
HX_CLOG_API int  hx_clog_context_put(const char* key, const char* value);
HX_CLOG_API void hx_clog_context_remove(const char* key);
HX_CLOG_API void hx_clog_context_clear(void);

/* -------------------------------------------------------------------------
 * Statistics
 * ------------------------------------------------------------------------- */
typedef struct hx_clog_stats {
    unsigned long long written_lines;
    unsigned long long dropped_lines;
    unsigned long long rotated_files;
    unsigned long long queue_high_watermark;
} hx_clog_stats_t;

HX_CLOG_API int hx_clog_get_stats(hx_clog_stats_t* stats);

/* -------------------------------------------------------------------------
 * Custom sinks
 * ------------------------------------------------------------------------- */
typedef int (*hx_clog_callback_t)(
    hx_clog_level_t level,
    const char* data,
    unsigned int size,
    void* user_data);

HX_CLOG_API int hx_clog_add_callback_sink(hx_clog_callback_t cb, void* user_data);
HX_CLOG_API int hx_clog_add_callback_sink_ex(hx_clog_callback_t cb,
                                             void* user_data,
                                             hx_clog_sink_id_t* out_id);
HX_CLOG_API int hx_clog_add_syslog_sink(const char* ident,
                                        hx_clog_sink_id_t* out_id);
HX_CLOG_API int hx_clog_add_event_log_sink(const char* source,
                                           hx_clog_sink_id_t* out_id);
HX_CLOG_API int hx_clog_add_android_log_sink(const char* tag,
                                             hx_clog_sink_id_t* out_id);
HX_CLOG_API int hx_clog_add_apple_log_sink(const char* subsystem,
                                           hx_clog_sink_id_t* out_id);
HX_CLOG_API int hx_clog_remove_sink(hx_clog_sink_id_t id);
HX_CLOG_API int hx_clog_set_sink_level(hx_clog_sink_id_t id,
                                       hx_clog_level_t min_level);
HX_CLOG_API int hx_clog_flush_sink(hx_clog_sink_id_t id);
HX_CLOG_API int hx_clog_get_sink_count(unsigned int* count);

/* Per-sink format override (added in 1.1.0).
 *
 * By default every sink receives the line rendered with the global
 * pattern/format mode. A sink can be given its own pattern and/or format
 * mode; the line is then rendered separately for that sink (sync and async).
 *
 * - hx_clog_set_sink_pattern(id, "...") gives the sink its own pattern and
 *   switches it to PATTERN mode. Passing NULL clears the whole override and
 *   the sink goes back to the global format.
 * - hx_clog_set_sink_format_mode(id, HX_CLOG_FORMAT_JSON) renders JSON for
 *   that sink only (a sink pattern set earlier is kept but unused in JSON
 *   mode).
 * Note: a global custom formatter does not apply to overridden sinks. */
HX_CLOG_API int hx_clog_set_sink_pattern(hx_clog_sink_id_t id,
                                         const char* pattern);
HX_CLOG_API int hx_clog_set_sink_format_mode(hx_clog_sink_id_t id,
                                             hx_clog_format_mode_t mode);

/* -------------------------------------------------------------------------
 * Internal error reporting (added in 1.1.0)
 *
 * The library never prints its own errors. Install a handler to be notified
 * about internal failures: sink creation failures during init/reconfigure,
 * file open / rotation failures, and async queue drops (throttled). The
 * handler may be called from any thread, including the async worker; keep it
 * fast and do not call back into hx_clog from it.
 * ------------------------------------------------------------------------- */
typedef void (*hx_clog_error_handler_t)(int err, const char* message,
                                        void* user_data);
HX_CLOG_API int hx_clog_set_error_handler(hx_clog_error_handler_t handler,
                                          void* user_data);

/* -------------------------------------------------------------------------
 * Duplicate suppression (added in 1.1.0)
 *
 * When enabled, consecutive log calls with the same level, source location
 * and message body are folded: the first occurrence is written, repeats
 * within window_ms are counted, and a single "last message repeated N times"
 * line is emitted when a different message arrives (or the window expires).
 * Disabled by default.
 * ------------------------------------------------------------------------- */
HX_CLOG_API int hx_clog_set_duplicate_suppression(int enable,
                                                  unsigned int window_ms);

/* -------------------------------------------------------------------------
 * Custom allocator (optional)
 * ------------------------------------------------------------------------- */
/* Note: the size parameter is unsigned int for ABI stability. Requests larger
 * than UINT_MAX (only possible with HX_CLOG_UNLIMITED_LINE) are refused when a
 * custom allocator is installed instead of being silently truncated. */
typedef void* (*hx_clog_malloc_fn)(unsigned int size, void* user_data);
typedef void  (*hx_clog_free_fn)(void* ptr, void* user_data);

typedef struct hx_clog_allocator {
    hx_clog_malloc_fn malloc_fn;
    hx_clog_free_fn   free_fn;
    void* user_data;
} hx_clog_allocator_t;

HX_CLOG_API int hx_clog_set_allocator(const hx_clog_allocator_t* allocator);

/* -------------------------------------------------------------------------
 * Crash handler
 * ------------------------------------------------------------------------- */
typedef struct hx_clog_crash_config {
    const char* crash_dir;
    int dump_fault_location;
    int dump_stacktrace;
    int dump_registers;
    int symbolize_stacktrace;
    int stacktrace_max_depth;
    const char* symbol_search_path;
    int create_minidump;       /* Windows only */
    int chain_previous_handler;
} hx_clog_crash_config_t;

HX_CLOG_API void hx_clog_crash_config_default(hx_clog_crash_config_t* config);
HX_CLOG_API int  hx_clog_install_crash_handler(const hx_clog_crash_config_t* config);
HX_CLOG_API void hx_clog_uninstall_crash_handler(void);

/* Optional user hook invoked from inside the crash handler after the report
 * body has been written (added in 1.1.0). `fd` is the open crash report file
 * descriptor: append your own context with low-level write() only. The
 * process is crashing — the callback must be async-signal-safe (no malloc,
 * no locks, no stdio). */
typedef void (*hx_clog_crash_callback_t)(int fd, void* user_data);
HX_CLOG_API int hx_clog_set_crash_callback(hx_clog_crash_callback_t cb,
                                           void* user_data);

/* -------------------------------------------------------------------------
 * fork support (Unix)
 *
 * Call in the child right after fork(). Re-initializes every internal lock
 * (core, ring buffer, file sinks, async queue) that may have been held by a
 * thread of the parent, and restarts the async worker thread (which does not
 * survive fork). Lines queued but not yet written in the parent are dropped
 * in the child.
 * ------------------------------------------------------------------------- */
HX_CLOG_API void hx_clog_after_fork_child(void);

/* -------------------------------------------------------------------------
 * Logging macros
 * ------------------------------------------------------------------------- */
/* Compile-time level cutting (à la spdlog's SPDLOG_ACTIVE_LEVEL). Define
 * HX_CLOG_ACTIVE_LEVEL to one of the HX_CLOG_LEVEL_NUM_* constants — e.g.
 * -DHX_CLOG_ACTIVE_LEVEL=HX_CLOG_LEVEL_NUM_INFO — and every macro below that
 * level expands to ((void)0): zero code, zero binary size, and the arguments
 * are never evaluated. Default keeps everything compiled in. The unified
 * `(...)` form works on every compiler (old MSVC included), so no version
 * split is needed. */
#define HX_CLOG_LEVEL_NUM_TRACE 0
#define HX_CLOG_LEVEL_NUM_DEBUG 1
#define HX_CLOG_LEVEL_NUM_INFO  2
#define HX_CLOG_LEVEL_NUM_WARN  3
#define HX_CLOG_LEVEL_NUM_ERROR 4
#define HX_CLOG_LEVEL_NUM_FATAL 5
#define HX_CLOG_LEVEL_NUM_OFF   6

#ifndef HX_CLOG_ACTIVE_LEVEL
#  define HX_CLOG_ACTIVE_LEVEL HX_CLOG_LEVEL_NUM_TRACE
#endif

/* HX_LOG_<L>_IF(cond, ...): log only when cond is true. HX_LOG_<L>_EVERY_N(n,
 * ...): log on the 1st and then every n-th call at that site (per-site static
 * counter; the count is approximate under concurrency — not atomic — matching
 * glog's default). When the level is compiled out, the condition / counter are
 * not evaluated at all. */

#if HX_CLOG_ACTIVE_LEVEL <= HX_CLOG_LEVEL_NUM_TRACE
#  define HX_LOG_TRACE(...)           hx_clog_write(HX_CLOG_LEVEL_TRACE, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOG_NAMED_TRACE(name,...) hx_clog_write_named(HX_CLOG_LEVEL_TRACE, name, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOGGER_TRACE(logger,...)  hx_clog_logger_write(logger, HX_CLOG_LEVEL_TRACE, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOG_TRACE_IF(cond,...)    do { if (cond) HX_LOG_TRACE(__VA_ARGS__); } while (0)
#  define HX_LOG_TRACE_EVERY_N(n,...)  do { static unsigned long hx_clog_n_=0; if ((hx_clog_n_++ % (unsigned long)(n))==0) HX_LOG_TRACE(__VA_ARGS__); } while (0)
#else
#  define HX_LOG_TRACE(...)            ((void)0)
#  define HX_LOG_NAMED_TRACE(name,...) ((void)0)
#  define HX_LOGGER_TRACE(logger,...)  ((void)0)
#  define HX_LOG_TRACE_IF(cond,...)    ((void)0)
#  define HX_LOG_TRACE_EVERY_N(n,...)  ((void)0)
#endif

#if HX_CLOG_ACTIVE_LEVEL <= HX_CLOG_LEVEL_NUM_DEBUG
#  define HX_LOG_DEBUG(...)           hx_clog_write(HX_CLOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOG_NAMED_DEBUG(name,...) hx_clog_write_named(HX_CLOG_LEVEL_DEBUG, name, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOGGER_DEBUG(logger,...)  hx_clog_logger_write(logger, HX_CLOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOG_DEBUG_IF(cond,...)    do { if (cond) HX_LOG_DEBUG(__VA_ARGS__); } while (0)
#  define HX_LOG_DEBUG_EVERY_N(n,...)  do { static unsigned long hx_clog_n_=0; if ((hx_clog_n_++ % (unsigned long)(n))==0) HX_LOG_DEBUG(__VA_ARGS__); } while (0)
#else
#  define HX_LOG_DEBUG(...)            ((void)0)
#  define HX_LOG_NAMED_DEBUG(name,...) ((void)0)
#  define HX_LOGGER_DEBUG(logger,...)  ((void)0)
#  define HX_LOG_DEBUG_IF(cond,...)    ((void)0)
#  define HX_LOG_DEBUG_EVERY_N(n,...)  ((void)0)
#endif

#if HX_CLOG_ACTIVE_LEVEL <= HX_CLOG_LEVEL_NUM_INFO
#  define HX_LOG_INFO(...)            hx_clog_write(HX_CLOG_LEVEL_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOG_NAMED_INFO(name,...) hx_clog_write_named(HX_CLOG_LEVEL_INFO, name, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOGGER_INFO(logger,...)  hx_clog_logger_write(logger, HX_CLOG_LEVEL_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOG_INFO_IF(cond,...)    do { if (cond) HX_LOG_INFO(__VA_ARGS__); } while (0)
#  define HX_LOG_INFO_EVERY_N(n,...)  do { static unsigned long hx_clog_n_=0; if ((hx_clog_n_++ % (unsigned long)(n))==0) HX_LOG_INFO(__VA_ARGS__); } while (0)
#else
#  define HX_LOG_INFO(...)            ((void)0)
#  define HX_LOG_NAMED_INFO(name,...) ((void)0)
#  define HX_LOGGER_INFO(logger,...)  ((void)0)
#  define HX_LOG_INFO_IF(cond,...)    ((void)0)
#  define HX_LOG_INFO_EVERY_N(n,...)  ((void)0)
#endif

#if HX_CLOG_ACTIVE_LEVEL <= HX_CLOG_LEVEL_NUM_WARN
#  define HX_LOG_WARN(...)            hx_clog_write(HX_CLOG_LEVEL_WARN, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOG_NAMED_WARN(name,...) hx_clog_write_named(HX_CLOG_LEVEL_WARN, name, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOGGER_WARN(logger,...)  hx_clog_logger_write(logger, HX_CLOG_LEVEL_WARN, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOG_WARN_IF(cond,...)    do { if (cond) HX_LOG_WARN(__VA_ARGS__); } while (0)
#  define HX_LOG_WARN_EVERY_N(n,...)  do { static unsigned long hx_clog_n_=0; if ((hx_clog_n_++ % (unsigned long)(n))==0) HX_LOG_WARN(__VA_ARGS__); } while (0)
#else
#  define HX_LOG_WARN(...)            ((void)0)
#  define HX_LOG_NAMED_WARN(name,...) ((void)0)
#  define HX_LOGGER_WARN(logger,...)  ((void)0)
#  define HX_LOG_WARN_IF(cond,...)    ((void)0)
#  define HX_LOG_WARN_EVERY_N(n,...)  ((void)0)
#endif

#if HX_CLOG_ACTIVE_LEVEL <= HX_CLOG_LEVEL_NUM_ERROR
#  define HX_LOG_ERROR(...)           hx_clog_write(HX_CLOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOG_NAMED_ERROR(name,...) hx_clog_write_named(HX_CLOG_LEVEL_ERROR, name, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOGGER_ERROR(logger,...)  hx_clog_logger_write(logger, HX_CLOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOG_ERROR_IF(cond,...)    do { if (cond) HX_LOG_ERROR(__VA_ARGS__); } while (0)
#  define HX_LOG_ERROR_EVERY_N(n,...)  do { static unsigned long hx_clog_n_=0; if ((hx_clog_n_++ % (unsigned long)(n))==0) HX_LOG_ERROR(__VA_ARGS__); } while (0)
#else
#  define HX_LOG_ERROR(...)           ((void)0)
#  define HX_LOG_NAMED_ERROR(name,...) ((void)0)
#  define HX_LOGGER_ERROR(logger,...)  ((void)0)
#  define HX_LOG_ERROR_IF(cond,...)    ((void)0)
#  define HX_LOG_ERROR_EVERY_N(n,...)  ((void)0)
#endif

#if HX_CLOG_ACTIVE_LEVEL <= HX_CLOG_LEVEL_NUM_FATAL
#  define HX_LOG_FATAL(...)           hx_clog_write(HX_CLOG_LEVEL_FATAL, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOG_NAMED_FATAL(name,...) hx_clog_write_named(HX_CLOG_LEVEL_FATAL, name, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOGGER_FATAL(logger,...)  hx_clog_logger_write(logger, HX_CLOG_LEVEL_FATAL, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOG_FATAL_IF(cond,...)    do { if (cond) HX_LOG_FATAL(__VA_ARGS__); } while (0)
#  define HX_LOG_FATAL_EVERY_N(n,...)  do { static unsigned long hx_clog_n_=0; if ((hx_clog_n_++ % (unsigned long)(n))==0) HX_LOG_FATAL(__VA_ARGS__); } while (0)
#else
#  define HX_LOG_FATAL(...)           ((void)0)
#  define HX_LOG_NAMED_FATAL(name,...) ((void)0)
#  define HX_LOGGER_FATAL(logger,...)  ((void)0)
#  define HX_LOG_FATAL_IF(cond,...)    ((void)0)
#  define HX_LOG_FATAL_EVERY_N(n,...)  ((void)0)
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HX_CLOG_H */
