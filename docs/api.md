# hx_clog API Reference

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
sinks.

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
cfg.rotate_on_startup = 1;          /* archive an existing active file at init */
```

`HX_CLOG_ROTATE_BY_TIME` keeps day rotation when no interval is configured; set
`rotate_interval_seconds` for hour/minute/second-style splits.

When zlib is available and `HX_CLOG_ENABLE_ZLIB=ON`, cleanup keeps the newest
`max_backup_files` plain rotated backups and compresses older plain backups to
`.gz` instead of deleting them. Compressed backups are still eligible for
`max_backup_days` age cleanup.

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
```

See [crash.md](crash.md) for details.

## Errors

`hx_clog_result_t` plus `const char* hx_clog_strerror(int err);`.

## Environment overrides

`HX_CLOG_LEVEL`, `HX_CLOG_DIR`, `HX_CLOG_MODE` (`sync`/`async`),
`HX_CLOG_CONSOLE` are applied during `hx_clog_init()`.
