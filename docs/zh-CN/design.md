# hx_clog 实现说明

[English](../design.md) | [中文](design.md)

本文说明源码文件如何对应到顶层 [README.zh-CN.md](../../README.zh-CN.md) 中描述的架构。

## 模块映射

| 文件 | 职责 |
| --- | --- |
| `include/hx_clog.h` | 公开 C ABI：类型、配置、生命周期、写日志接口、宏 |
| `include/hx_clog_cpp.hpp` | 可选 C++11 RAII 封装，不导出 C++ ABI |
| `src/hx_clog_internal.h` | 内部共享声明与平台抽象 |
| `src/hx_clog_core.c` | 全局状态、命名 logger、线程本地 context、级别过滤、写入路径、sink 列表、统计、allocator、线程/平台辅助函数、crash ring buffer |
| `src/hx_clog_format.c` | Pattern 引擎（`%Y %m %d %H %M %S %e %l %c %t %p %s %# %! %v %x %n %%`）和 JSON formatter |
| `src/hx_clog_sink.c` | Sink 分发，以及 console、callback、syslog、Event Log、logcat、Apple system sink |
| `src/hx_clog_file.c` | 带缓冲的 file sink 和 reopen |
| `src/hx_clog_file.h` | File sink 与轮转共享的状态 |
| `src/hx_clog_rotate.c` | 按大小、按天、按时间间隔、启动时轮转，归档命名，按时间清理，超过普通文件保留数后的旧备份压缩 |
| `src/hx_clog_async.c` | 有界 ring queue 和后台 worker，在 `HX_CLOG_ENABLE_ASYNC` 开启时构建 |
| `src/hx_clog_crash.c` | SEH / POSIX signal crash 捕获，在 `HX_CLOG_ENABLE_CRASH` 开启时构建 |

## 写入路径

```text
HX_LOG_INFO(...)
  -> 级别过滤（原子加载，发生在所有工作之前）
  -> vsnprintf 用户消息（栈缓冲，长行使用堆 fallback）
  -> 捕获 logger 名称和线程本地 context
  -> custom formatter、JSON formatter，或 hx_format_record() 组装整行
  -> 写入 crash ring buffer（低成本、固定内存）
  -> SYNC : 在 sink lock 下写入每个 sink
     ASYNC: 拷贝进队列，由 worker 批量 drain
  -> FATAL: 立即 flush
```

## 线程模型

* 单个进程级 logger（singleton），由 `once` 初始化器保护。
* Level 是 relaxed atomic int，用于低成本热路径过滤。
* 同步写入由一个 mutex 串行化；file sink 也有自己的 mutex，确保轮转安全。
* 异步模式下，业务线程只拷贝日志到预分配 ring queue；一个 worker 线程按间隔写入 sink 并 flush。

## 平台抽象

`hx_clog_internal.h` 声明轻量封装（`hx_mutex_*`、`hx_cond_*`、`hx_thread_*`、time、pid/tid、filesystem），由 `hx_clog_core.c` 实现。Windows 分支使用 `CRITICAL_SECTION`、`CONDITION_VARIABLE`、`_beginthreadex`、`GetSystemTimeAsFileTime`，POSIX 分支使用 `pthread`、`gettimeofday`、`nanosleep`。

## 内存

* 短消息使用 1 KB 栈缓冲；更长的消息使用一次堆分配 fallback，并受 `HX_CLOG_MAX_LINE` 限制。
* 异步队列和 crash ring buffer 在 init 时预分配。
* 用户 allocator 可在 init 前通过 `hx_clog_set_allocator` 安装。
