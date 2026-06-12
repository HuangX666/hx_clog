# hx_clog API Reference

[English](api.md) | [中文](zh-CN/api.md)

All functions use the C ABI and are declared in `include/hx_clog.h`.
The optional C++11 wrapper lives in `include/hx_clog_cpp.hpp`.
Build and CI notes are in [ci.md](ci.md).

## Lifecycle

```c
void hx_clog_config_default(hx_clog_config_t* config);
int  hx_clog_init(const hx_clog_config_t* config);   /* HX_CLOG_OK on success */
void hx_clog_shutdown(void);                          /* drains async + closes sinks */
void hx_clog_flush(void);
int  hx_clog_is_initialized(void);
int  hx_clog_reconfigure(const hx_clog_config_t* config);
int  hx_clog_reopen(void);                            /* after logrotate / file move */
```

Call `hx_clog_config_default()` first, override the fields you care about, then
`hx_clog_init()`. `hx_clog_shutdown()` is safe to call once; extra calls are
no-ops. In async mode shutdown always drains the queue before closing files.
`hx_clog_reconfigure()` updates the level, default logger name, format mode,
pattern/custom formatter, mode, and built-in sinks while preserving callback
sinks. Since 1.1.0 the file sink swap is **fail-safe**: the new file sink is
created first, and if that fails (bad `log_dir`, no permission) the call
returns `HX_CLOG_ERR_OPEN_FILE_FAILED` with **all previous sinks left
untouched** — a typo in a new log directory can no longer silence file output.

### Log directory and file name

* `log_dir` may be a multi-level relative or absolute path (e.g. `"test/logs"`,
  `"./var/log/app"`). `hx_clog_init()` **recursively creates** any missing
  intermediate directories (both `/` and `\` separators are recognized;
  existing directories are skipped).
* `file_name` must be a plain file name (e.g. `"app.log"`). Do **not** embed
  subdirectories in `file_name` — put them in `log_dir` instead:

  ```c
  cfg.log_dir   = "test/logs";   /* test/ and test/logs/ are created */
  cfg.file_name = "xxx.log";
  ```

`hx_clog_init()` rejects `file_name` values containing `/`, `\`, or `:`.

All paths are interpreted as **UTF-8** on every platform. On Windows they are
converted to UTF-16 internally and the wide file APIs are used, so non-ASCII
(e.g. Chinese) directories and file names work regardless of the system ANSI
codepage.

## Levels

```c
HX_CLOG_LEVEL_TRACE, _DEBUG, _INFO, _WARN, _ERROR, _FATAL, _OFF

void            hx_clog_set_level(hx_clog_level_t level);  /* atomic */
hx_clog_level_t hx_clog_get_level(void);
const char*     hx_clog_level_name(hx_clog_level_t level);
hx_clog_level_t hx_clog_level_from_name(const char* name); /* "info", "warn" ... */
```

Level filtering happens before any formatting or allocation, so disabled log
statements are nearly free.

## Writing logs

Prefer the macros — they capture `__FILE__`, `__LINE__`, `__func__`:

```c
HX_LOG_TRACE(fmt, ...);
HX_LOG_DEBUG(fmt, ...);
HX_LOG_INFO (fmt, ...);
HX_LOG_WARN (fmt, ...);
HX_LOG_ERROR(fmt, ...);
HX_LOG_FATAL(fmt, ...);
```

Lower-level entry points:

```c
void hx_clog_write (hx_clog_level_t lvl, const char* file, int line,
                    const char* func, const char* fmt, ...);
void hx_clog_writev(hx_clog_level_t lvl, const char* file, int line,
                    const char* func, const char* fmt, va_list args);
void hx_clog_write_named(hx_clog_level_t lvl, const char* logger_name,
                         const char* file, int line, const char* func,
                         const char* fmt, ...);
```

`FATAL` records are flushed immediately to maximise crash survivability.

## Named Loggers and Context

```c
hx_clog_logger_t* hx_clog_default_logger(void);
int  hx_clog_logger_create(const char* name, hx_clog_level_t level,
                           hx_clog_logger_t** out_logger);
void hx_clog_logger_destroy(hx_clog_logger_t* logger);
int  hx_clog_logger_set_level(hx_clog_logger_t* logger, hx_clog_level_t level);

HX_LOG_NAMED_INFO("net", "connected");
HX_LOGGER_ERROR(logger, "worker failed: %d", rc);
```

Named loggers share the process-wide sinks but carry their own logger/category
name and minimum level. The global level remains a process-wide floor.

Thread-local context:

```c
hx_clog_context_put("request_id", "42");
hx_clog_context_remove("request_id");
hx_clog_context_clear();
```

Pattern mode supports `%c` for logger/category and `%x` for context.

## Formatting

```c
cfg.format_mode = HX_CLOG_FORMAT_JSON;
hx_clog_set_format_mode(HX_CLOG_FORMAT_JSON);
hx_clog_set_pattern("%Y-%m-%d [%c] %x %v%n");
```

Custom formatters receive the public `hx_clog_record_t`:

```c
unsigned int my_formatter(const hx_clog_record_t* rec, char* out,
                          unsigned int out_size, void* user);
hx_clog_set_formatter(my_formatter, user);
```

## Statistics

```c
typedef struct hx_clog_stats {
    unsigned long long written_lines;
    unsigned long long dropped_lines;
    unsigned long long rotated_files;
    unsigned long long queue_high_watermark;
} hx_clog_stats_t;

int hx_clog_get_stats(hx_clog_stats_t* stats);
```

## Custom sinks

```c
typedef int (*hx_clog_callback_t)(hx_clog_level_t level, const char* data,
                                  unsigned int size, void* user_data);
int hx_clog_add_callback_sink(hx_clog_callback_t cb, void* user_data);
int hx_clog_add_callback_sink_ex(hx_clog_callback_t cb, void* user_data,
                                 hx_clog_sink_id_t* out_id);
```

Add callback sinks **after** `hx_clog_init()` — init rebuilds the sink list.

Sink management:

```c
int hx_clog_remove_sink(hx_clog_sink_id_t id);
int hx_clog_set_sink_level(hx_clog_sink_id_t id, hx_clog_level_t min_level);
int hx_clog_flush_sink(hx_clog_sink_id_t id);
int hx_clog_get_sink_count(unsigned int* count);
```

Per-sink format override (1.1.0):

```c
/* this sink renders with its own pattern (switches it to PATTERN mode) */
int hx_clog_set_sink_pattern(hx_clog_sink_id_t id, const char* pattern);
/* this sink renders JSON (or back to PATTERN) regardless of the global mode */
int hx_clog_set_sink_format_mode(hx_clog_sink_id_t id, hx_clog_format_mode_t mode);
```

Passing `NULL` to `hx_clog_set_sink_pattern()` clears the whole override and
the sink returns to the global format. Overrides work in both sync and async
mode; a global custom formatter does not apply to overridden sinks.

System sinks:

```c
int hx_clog_add_syslog_sink(const char* ident, hx_clog_sink_id_t* out_id);
int hx_clog_add_event_log_sink(const char* source, hx_clog_sink_id_t* out_id);
int hx_clog_add_android_log_sink(const char* tag, hx_clog_sink_id_t* out_id);
int hx_clog_add_apple_log_sink(const char* subsystem, hx_clog_sink_id_t* out_id);
```

Unsupported platform/system sink combinations return `HX_CLOG_ERR_PLATFORM`.
On Unix/Apple targets, syslog support is compiled when
`HX_CLOG_ENABLE_SYSLOG=ON`.

## Rotation

```c
cfg.rotate_interval_seconds = 3600; /* interval time rotation, 0=off */
cfg.rotate_align = 1;               /* 1.1.0: align to wall-clock boundaries */
cfg.rotate_on_startup = 1;          /* archive an existing active file at init */
cfg.max_compressed_files = 20;      /* 1.1.0: cap .gz backups; 0=max_backup_files */
```

`HX_CLOG_ROTATE_BY_TIME` keeps day rotation when no interval is configured; set
`rotate_interval_seconds` for hour/minute/second-style splits. With
`rotate_align = 1` the interval is aligned to wall-clock buckets (epoch based),
so `3600` rotates **on the hour** instead of "one hour after the file was
opened".

Archive names carry the date the active file was **opened** — a file written on
Friday and rotated on Monday is archived under Friday's date.

When zlib is available and `HX_CLOG_ENABLE_ZLIB=ON`, cleanup keeps the newest
`max_backup_files` plain rotated backups and compresses older plain backups to
`.gz` instead of deleting them. The number of `.gz` backups is itself capped by
`max_compressed_files` (defaulting to `max_backup_files`), so compressed
backups cannot accumulate forever; the oldest are deleted first. Compressed
backups are still eligible for `max_backup_days` age cleanup.

## Custom allocator

```c
int hx_clog_set_allocator(const hx_clog_allocator_t* allocator);
```

Must be called before `hx_clog_init()`.

## Crash handler

```c
void hx_clog_crash_config_default(hx_clog_crash_config_t* config);
int  hx_clog_install_crash_handler(const hx_clog_crash_config_t* config);
void hx_clog_uninstall_crash_handler(void);

/* 1.1.0: append app context to the report from inside the handler.
 * Must be async-signal-safe: write(fd, ...) only. */
int  hx_clog_set_crash_callback(hx_clog_crash_callback_t cb, void* user_data);
```

See [crash.md](crash.md) for details.

## Internal error handler (1.1.0)

```c
typedef void (*hx_clog_error_handler_t)(int err, const char* message, void* ud);
int hx_clog_set_error_handler(hx_clog_error_handler_t handler, void* user_data);
```

The library never prints its own errors. The handler is invoked on sink
creation failures during init/reconfigure, file open/rotation failures
(including a failed archive rename — e.g. the file is locked by another
process), file write failures (disk full; reported once per failure episode,
not per line), and async queue drops (first drop, then every 10000th). It may
run on any thread — keep it fast and never call back into hx_clog from it.

## Duplicate suppression (1.1.0)

```c
int hx_clog_set_duplicate_suppression(int enable, unsigned int window_ms);
```

When enabled, consecutive calls with the same level, source location and
message are folded into the first line plus a single
`last message repeated N times` summary, emitted when a different message
arrives, on flush/shutdown, or when the window expires. Disabled by default.

## fork support

```c
void hx_clog_after_fork_child(void);
```

Call in the child right after `fork()`. Re-initializes every internal lock
(core, ring buffer, file sinks, async queue) and restarts the async worker
thread; lines queued but unwritten in the parent are dropped in the child.

## Errors

`hx_clog_result_t` plus `const char* hx_clog_strerror(int err);`.

## Environment overrides

`HX_CLOG_LEVEL`, `HX_CLOG_DIR`, `HX_CLOG_MODE` (`sync`/`async`),
`HX_CLOG_CONSOLE` are applied during `hx_clog_init()`.
