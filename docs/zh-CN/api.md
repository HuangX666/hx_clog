# hx_clog API 参考

[English](../api.md) | [中文](api.md)

所有函数都使用 C ABI，并在 `include/hx_clog.h` 中声明。
可选 C++11 封装位于 `include/hx_clog_cpp.hpp`。
构建和 CI 说明见 [ci.md](ci.md)。

## 生命周期

```c
void hx_clog_config_default(hx_clog_config_t* config);
int  hx_clog_init(const hx_clog_config_t* config);   /* HX_CLOG_OK on success */
int  hx_clog_init_from_file(const char* path);        /* 从 INI 文件初始化 */
void hx_clog_shutdown(void);                          /* drains async + closes sinks */
void hx_clog_flush(void);                             /* 正常退出时也会执行（atexit） */
int  hx_clog_is_initialized(void);
int  hx_clog_reconfigure(const hx_clog_config_t* config);
int  hx_clog_get_config(hx_clog_config_t* out_config); /* 读回当前生效配置 */
int  hx_clog_reopen(void);                            /* after logrotate / file move */
```

先调用 `hx_clog_config_default()`，再覆盖关心的字段，然后调用 `hx_clog_init()`。`hx_clog_shutdown()` 调用一次是安全的；额外调用是 no-op。异步模式下，shutdown 总会先 drain 队列再关闭文件。

`hx_clog_init_from_file()` 解析一个 INI 文件（`[hx_clog]` 段的 `key=value` 行；键对应配置字段；`#`/`;` 注释；尺寸支持 `K`/`M`/`G`）后调用 `hx_clog_init()`，文件里没有的键保持默认值（可识别的键见 README §19）。`hx_clog_get_config()` 把当前生效配置（反映环境变量覆盖与 async→sync 回退）拷贝到 `out_config`，返回的字符串指针在下次 init/reconfigure 前有效。由于 `hx_clog_flush()` 也通过 `atexit()` 注册，正常退出时即使没显式调用 `hx_clog_shutdown()` 也会冲刷异步/缓冲日志。

`hx_clog_reconfigure()` 会更新 level、默认 logger 名称、format mode、pattern/custom formatter、mode 和内置 sinks，同时保留 callback sinks。自 1.1.0 起，file sink 切换是 **fail-safe** 的：先创建新 file sink，如果失败（错误的 `log_dir`、无权限等），函数返回 `HX_CLOG_ERR_OPEN_FILE_FAILED`，且 **所有旧 sink 保持不变**，因此新的日志目录拼错不会让文件输出静默失效。

### 日志目录和文件名

* `log_dir` 可以是多级相对或绝对路径，例如 `"test/logs"`、`"./var/log/app"`。`hx_clog_init()` 会 **递归创建** 缺失的中间目录，同时识别 `/` 和 `\` 分隔符，已存在目录会跳过。
* `file_name` 必须是普通文件名，例如 `"app.log"`。不要在 `file_name` 中嵌入子目录；请把目录放到 `log_dir`：

  ```c
  cfg.log_dir   = "test/logs";   /* test/ and test/logs/ are created */
  cfg.file_name = "xxx.log";
  ```

`hx_clog_init()` 会拒绝包含 `/`、`\` 或 `:` 的 `file_name`。

所有平台上的路径都解释为 **UTF-8**。Windows 内部会转换为 UTF-16 并使用宽字符文件 API，因此非 ASCII 目录和文件名（例如中文）不受系统 ANSI codepage 影响。

## 级别

```c
HX_CLOG_LEVEL_TRACE, _DEBUG, _INFO, _WARN, _ERROR, _FATAL, _OFF

void            hx_clog_set_level(hx_clog_level_t level);  /* atomic */
hx_clog_level_t hx_clog_get_level(void);
const char*     hx_clog_level_name(hx_clog_level_t level);
hx_clog_level_t hx_clog_level_from_name(const char* name); /* "info", "warn" ... */
```

级别过滤发生在任何格式化或分配之前，因此被禁用的日志语句开销极低。

## 写日志

优先使用宏，因为它们会捕获 `__FILE__`、`__LINE__`、`__func__`：

```c
HX_LOG_TRACE(fmt, ...);
HX_LOG_DEBUG(fmt, ...);
HX_LOG_INFO (fmt, ...);
HX_LOG_WARN (fmt, ...);
HX_LOG_ERROR(fmt, ...);
HX_LOG_FATAL(fmt, ...);
```

每个级别都有**条件 / 采样**变体（glog 风格）：

```c
HX_LOG_WARN_IF(cond, fmt, ...);       /* 仅当 cond 为真时输出           */
HX_LOG_INFO_EVERY_N(n, fmt, ...);     /* 第 1 次，之后每 n 次（按调用点计数，
                                       *  并发下为近似值）*/
```

**编译期级别裁剪。** 在 include 头文件之前定义 `HX_CLOG_ACTIVE_LEVEL` 为
`HX_CLOG_LEVEL_NUM_TRACE … HX_CLOG_LEVEL_NUM_OFF` 之一（或 `-DHX_CLOG_ACTIVE_LEVEL=...`）。
门槛以下的宏（含其 `_IF` / `_EVERY_N` 变体）展开为 `((void)0)`：不生成代码，参数和条件
也不会被求值。这与运行时 `hx_clog_set_level()` 过滤相互独立——一条日志必须同时通过两者。

```c
#define HX_CLOG_ACTIVE_LEVEL HX_CLOG_LEVEL_NUM_INFO
#include "hx_clog.h"
```

更底层的入口：

```c
void hx_clog_write (hx_clog_level_t lvl, const char* file, int line,
                    const char* func, const char* fmt, ...);
void hx_clog_writev(hx_clog_level_t lvl, const char* file, int line,
                    const char* func, const char* fmt, va_list args);
void hx_clog_write_named(hx_clog_level_t lvl, const char* logger_name,
                         const char* file, int line, const char* func,
                         const char* fmt, ...);
```

`FATAL` 记录会立即 flush，以尽量提高 crash 场景下的可存活性。

## 命名 Logger 和 Context

```c
hx_clog_logger_t* hx_clog_default_logger(void);
int  hx_clog_logger_create(const char* name, hx_clog_level_t level,
                           hx_clog_logger_t** out_logger);
void hx_clog_logger_destroy(hx_clog_logger_t* logger);
int  hx_clog_logger_set_level(hx_clog_logger_t* logger, hx_clog_level_t level);

HX_LOG_NAMED_INFO("net", "connected");
HX_LOGGER_ERROR(logger, "worker failed: %d", rc);
```

命名 logger 共享进程级 sinks，但拥有自己的 logger/category 名称和最低级别。全局 level 仍然是进程级下限。

### 注册表与点分名层级

```c
hx_clog_logger_t* hx_clog_logger_get(const char* name);   /* 取或创建+注册 */
hx_clog_logger_t* hx_clog_logger_find(const char* name);  /* 不存在返回 NULL */
unsigned int      hx_clog_logger_count(void);
int  hx_clog_set_level_for_prefix(const char* prefix, hx_clog_level_t level);
void hx_clog_logger_drop_all(void);
```

`hx_clog_logger_get()` 返回已有的同名 logger，或创建并**注册**一个新的——无需保存指针。
新建的 `"a.b.c"` 继承最近已注册祖先（`"a.b"`，再 `"a"`）的级别，否则用全局级别。
`hx_clog_set_level_for_prefix()` 设置前缀 logger 及其所有现有后代（`"net"` 影响 `"net"`、
`"net.tcp"` …），并创建前缀 logger，使**之后**创建的后代也能继承。注册表 logger 一直存活
到 `hx_clog_logger_drop_all()` 或进程退出——**不要**把它们传给 `hx_clog_logger_destroy()`
（那只用于 `hx_clog_logger_create()` 创建的 logger）。

线程本地 context：

```c
hx_clog_context_put("request_id", "42");
hx_clog_context_remove("request_id");
hx_clog_context_clear();
```

Pattern 模式支持 `%c` 输出 logger/category，`%x` 输出 context。

## 格式化

```c
cfg.format_mode = HX_CLOG_FORMAT_JSON;
hx_clog_set_format_mode(HX_CLOG_FORMAT_JSON);
hx_clog_set_pattern("%Y-%m-%d [%c] %x %v%n");
```

自定义 formatter 接收公开的 `hx_clog_record_t`：

```c
unsigned int my_formatter(const hx_clog_record_t* rec, char* out,
                          unsigned int out_size, void* user);
hx_clog_set_formatter(my_formatter, user);
```

## 统计

```c
typedef struct hx_clog_stats {
    unsigned long long written_lines;
    unsigned long long dropped_lines;
    unsigned long long rotated_files;
    unsigned long long queue_high_watermark;
} hx_clog_stats_t;

int hx_clog_get_stats(hx_clog_stats_t* stats);
```

## 自定义 sinks

```c
typedef int (*hx_clog_callback_t)(hx_clog_level_t level, const char* data,
                                  unsigned int size, void* user_data);
int hx_clog_add_callback_sink(hx_clog_callback_t cb, void* user_data);
int hx_clog_add_callback_sink_ex(hx_clog_callback_t cb, void* user_data,
                                 hx_clog_sink_id_t* out_id);
```

请在 `hx_clog_init()` **之后** 添加 callback sink，因为 init 会重建 sink 列表。

Sink 管理：

```c
int hx_clog_remove_sink(hx_clog_sink_id_t id);
int hx_clog_set_sink_level(hx_clog_sink_id_t id, hx_clog_level_t min_level);
int hx_clog_flush_sink(hx_clog_sink_id_t id);
int hx_clog_get_sink_count(unsigned int* count);
```

Per-sink 格式覆盖（1.1.0）：

```c
/* this sink renders with its own pattern (switches it to PATTERN mode) */
int hx_clog_set_sink_pattern(hx_clog_sink_id_t id, const char* pattern);
/* this sink renders JSON (or back to PATTERN) regardless of the global mode */
int hx_clog_set_sink_format_mode(hx_clog_sink_id_t id, hx_clog_format_mode_t mode);
```

向 `hx_clog_set_sink_pattern()` 传入 `NULL` 会清除整个 override，使该 sink 回到全局格式。Override 在同步和异步模式下都可用；全局 custom formatter 不会应用到被 override 的 sink。

系统 sinks：

```c
int hx_clog_add_syslog_sink(const char* ident, hx_clog_sink_id_t* out_id);
int hx_clog_add_event_log_sink(const char* source, hx_clog_sink_id_t* out_id);
int hx_clog_add_android_log_sink(const char* tag, hx_clog_sink_id_t* out_id);
int hx_clog_add_apple_log_sink(const char* subsystem, hx_clog_sink_id_t* out_id);
```

不支持的平台/系统 sink 组合返回 `HX_CLOG_ERR_PLATFORM`。Unix/Apple 目标上，`HX_CLOG_ENABLE_SYSLOG=ON` 时会编译 syslog 支持。

网络 sink（`HX_CLOG_ENABLE_NET`，默认开）：

```c
typedef enum hx_clog_net_protocol { HX_CLOG_NET_TCP = 0, HX_CLOG_NET_UDP } hx_clog_net_protocol_t;
int hx_clog_add_network_sink(hx_clog_net_protocol_t protocol, const char* host,
                             unsigned short port, hx_clog_sink_id_t* out_id);
```

把每一行发送到 `host:port`。TCP 保持连接并在失败后限速重连；UDP fire-and-forget。
首次写入时才惰性建连，所以添加 sink 不会阻塞、collector 不可用也不会导致 init 失败；
链路断开期间丢行（由上游异步队列缓冲），并每个重试窗口最多通知一次 error handler。
建议与异步模式搭配，让 socket IO 跑在后台 worker 上。未启用 `HX_CLOG_ENABLE_NET` 时返回
`HX_CLOG_ERR_PLATFORM`。

## 轮转

```c
cfg.rotate_interval_seconds = 3600; /* interval time rotation, 0=off */
cfg.rotate_align = 1;               /* 1.1.0: align to wall-clock boundaries */
cfg.rotate_on_startup = 1;          /* archive an existing active file at init */
cfg.max_compressed_files = 20;      /* cap .gz backups; -1=无上限（默认） */
cfg.date_subdir = 1;               /* 1.2.0: 按天分目录，见下 */
```

未配置 interval 时，`HX_CLOG_ROTATE_BY_TIME` 保持按天轮转；设置 `rotate_interval_seconds` 可获得按小时/分钟/秒风格的切分。`rotate_align = 1` 时，interval 会对齐到基于 epoch 的墙钟时间桶，因此 `3600` 表示 **整点** 轮转，而不是“文件打开后一个小时”。

归档名带有 active 文件 **打开** 的日期，因此周五写入、周一轮转的文件会归档到周五日期下。cleanup 按文件名里嵌入的 `日期+序号`（每次轮转都精确）排序，而不是按文件 mtime，所以即使大量轮转发生在同一秒内，也能正确保留真正最新的备份。

### 保留策略（`max_backup_files` / `max_compressed_files`）

| 字段 | 含义 | 取值 |
| --- | --- | --- |
| `max_backup_files` | 保留多少个最新备份为**未压缩明文** | `-1` 全部保留为明文、永不压缩、永不删除（**默认**）；`0` 永不压缩；`N>0` 留 N 个明文，更旧的压缩 |
| `max_compressed_files` | `.gz` 备份数量上限 | `-1` 永不按数量删除（**默认**）；`0` 沿用 `max_backup_files`；`N>0` 保留最新 N 个 `.gz` |
| `max_backup_days` | 删除超过 N 天的备份（明文或 `.gz`） | `0` 关闭（默认） |

因此默认值（`-1` / `-1` / `0`）**什么都不删**——每个轮转文件都以明文保留。这是最不容易丢数据的设置，但磁盘占用会无限增长；想要有界策略，请设一个有限的 `max_backup_files`（启用压缩）和/或 `max_backup_days`（按天龄清理）。例如「留 2 个明文、其余压缩、gz 全留」：

```c
cfg.max_backup_files = 2;
cfg.max_compressed_files = -1;   /* 或一个有限上限 */
cfg.max_backup_days = 30;        /* 可选的天龄兜底 */
```

zlib 可用（`HX_CLOG_ENABLE_ZLIB=ON`）时，超过 `max_backup_files` 的备份会被压缩为 `.gz`；没有 zlib 时则直接删除。

### 按天分目录（`date_subdir`，1.2.0）

设 `cfg.date_subdir = 1`，active 日志会写入按天命名的 `YYYY-MM-DD` 子文件夹：

```
./logs/2026-06-13/app.log
./logs/2026-06-13/app.2026-06-13.1.log   （大小轮转的归档也留在当天文件夹里）
./logs/2026-06-14/app.log                （跨天自动切到新文件夹）
```

日期文件夹自动创建。跨天时 sink 直接切到新一天的文件夹，前一天的文件原样保留（不再 rename 归档）。大小轮转仍在每天的文件夹内生效。`file_name` 仍必须是纯文件名——日期段加在目录上，而非文件名里。

## 自定义 allocator

```c
int hx_clog_set_allocator(const hx_clog_allocator_t* allocator);
```

必须在 `hx_clog_init()` 之前调用。

## Crash handler

```c
void hx_clog_crash_config_default(hx_clog_crash_config_t* config);
int  hx_clog_install_crash_handler(const hx_clog_crash_config_t* config);
void hx_clog_uninstall_crash_handler(void);

/* 1.1.0: append app context to the report from inside the handler.
 * Must be async-signal-safe: write(fd, ...) only. */
int  hx_clog_set_crash_callback(hx_clog_crash_callback_t cb, void* user_data);
```

详情见 [crash.md](crash.md)。

## 内部错误处理器 (1.1.0)

```c
typedef void (*hx_clog_error_handler_t)(int err, const char* message, void* ud);
int hx_clog_set_error_handler(hx_clog_error_handler_t handler, void* user_data);
```

库不会自行打印错误。以下场景会触发 handler：init/reconfigure 期间 sink 创建失败、文件打开/轮转失败（包括归档 rename 失败，例如文件被其他进程锁住）、文件写入失败（磁盘满；每次失败 episode 只报告一次，不按每行报告）、异步队列丢弃（第一次丢弃以及之后每 10000 次）。它可能在任意线程运行，所以应保持快速，并且不要从 handler 回调 hx_clog。

## 重复日志折叠 (1.1.0)

```c
int hx_clog_set_duplicate_suppression(int enable, unsigned int window_ms);
```

开启后，连续调用中 level、源码位置和消息都相同的日志会折叠为第一行加一条 `last message repeated N times` 摘要。摘要会在出现不同消息、flush/shutdown 或窗口过期时输出。默认关闭。

## fork 支持

```c
void hx_clog_after_fork_child(void);
```

在 child 进程 `fork()` 后立即调用。该函数会重新初始化所有内部锁（core、ring buffer、file sinks、async queue）并重启 async worker；父进程中已入队但尚未写出的日志会在 child 中丢弃。

## 错误

`hx_clog_result_t` 以及 `const char* hx_clog_strerror(int err);`。

## 环境变量覆盖

`HX_CLOG_LEVEL`、`HX_CLOG_DIR`、`HX_CLOG_MODE`（`sync`/`async`）、`HX_CLOG_CONSOLE` 会在 `hx_clog_init()` 期间应用。

