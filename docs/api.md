# hx_clog API Reference

All functions use the C ABI and are declared in `include/hx_clog.h`.
The optional C++11 wrapper lives in `include/hx_clog_cpp.hpp`.

## Lifecycle

```c
void hx_clog_config_default(hx_clog_config_t* config);
int  hx_clog_init(const hx_clog_config_t* config);   /* HX_CLOG_OK on success */
void hx_clog_shutdown(void);                          /* drains async + closes sinks */
void hx_clog_flush(void);
int  hx_clog_is_initialized(void);
int  hx_clog_reopen(void);                            /* after logrotate / file move */
```

Call `hx_clog_config_default()` first, override the fields you care about, then
`hx_clog_init()`. `hx_clog_shutdown()` is safe to call once; extra calls are
no-ops. In async mode shutdown always drains the queue before closing files.

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
```

`FATAL` records are flushed immediately to maximise crash survivability.

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
```

Add callback sinks **after** `hx_clog_init()` — init rebuilds the sink list.

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
