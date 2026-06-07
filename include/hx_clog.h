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
#define HX_CLOG_VERSION_MINOR 0
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

    hx_clog_rotate_policy_t rotate_policy;
    unsigned long long max_file_size;  /* e.g. 10 * 1024 * 1024 */
    int max_backup_files;              /* e.g. 10 */
    int max_backup_days;               /* keep files for at most N days, 0=off */
    int rotate_daily;                  /* split by day */

    unsigned int async_queue_size;     /* default 8192 */
    unsigned int async_batch_size;     /* default 64 */
    unsigned int flush_interval_ms;    /* default 1000 */
    hx_clog_overflow_policy_t overflow_policy; /* default BLOCK */

    const char* pattern;               /* default built-in pattern */
} hx_clog_config_t;

HX_CLOG_API void hx_clog_config_default(hx_clog_config_t* config);

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */
HX_CLOG_API int  hx_clog_init(const hx_clog_config_t* config);
HX_CLOG_API void hx_clog_shutdown(void);
HX_CLOG_API void hx_clog_flush(void);
HX_CLOG_API int  hx_clog_is_initialized(void);

HX_CLOG_API void            hx_clog_set_level(hx_clog_level_t level);
HX_CLOG_API hx_clog_level_t hx_clog_get_level(void);

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

/* -------------------------------------------------------------------------
 * Custom allocator (optional)
 * ------------------------------------------------------------------------- */
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

/* -------------------------------------------------------------------------
 * fork support (Unix)
 * ------------------------------------------------------------------------- */
HX_CLOG_API void hx_clog_after_fork_child(void);

/* -------------------------------------------------------------------------
 * Logging macros
 * ------------------------------------------------------------------------- */
#if defined(_MSC_VER) && !defined(__clang__) && (_MSC_VER < 1920)
/* Old MSVC: provide a traditional variadic macro form. */
#  define HX_LOG_TRACE(...) hx_clog_write(HX_CLOG_LEVEL_TRACE, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOG_DEBUG(...) hx_clog_write(HX_CLOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOG_INFO(...)  hx_clog_write(HX_CLOG_LEVEL_INFO,  __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOG_WARN(...)  hx_clog_write(HX_CLOG_LEVEL_WARN,  __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOG_ERROR(...) hx_clog_write(HX_CLOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define HX_LOG_FATAL(...) hx_clog_write(HX_CLOG_LEVEL_FATAL, __FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#  define HX_LOG_TRACE(fmt, ...) \
    hx_clog_write(HX_CLOG_LEVEL_TRACE, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#  define HX_LOG_DEBUG(fmt, ...) \
    hx_clog_write(HX_CLOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#  define HX_LOG_INFO(fmt, ...) \
    hx_clog_write(HX_CLOG_LEVEL_INFO, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#  define HX_LOG_WARN(fmt, ...) \
    hx_clog_write(HX_CLOG_LEVEL_WARN, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#  define HX_LOG_ERROR(fmt, ...) \
    hx_clog_write(HX_CLOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#  define HX_LOG_FATAL(fmt, ...) \
    hx_clog_write(HX_CLOG_LEVEL_FATAL, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HX_CLOG_H */
