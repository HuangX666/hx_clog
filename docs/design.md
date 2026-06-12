# hx_clog Implementation Notes

[English](design.md) | [中文](zh-CN/design.md)

This document describes how the source files map to the architecture in the
top-level `README.md`.

## Module map

| File | Responsibility |
| --- | --- |
| `include/hx_clog.h` | Public C ABI: types, config, lifecycle, write, macros |
| `include/hx_clog_cpp.hpp` | Optional C++11 RAII wrapper (no C++ ABI exported) |
| `src/hx_clog_internal.h` | Shared internal declarations + platform abstractions |
| `src/hx_clog_core.c` | Global state, named loggers, thread-local context, level filtering, write path, sink list, stats, allocator, threading/platform helpers, crash ring buffer |
| `src/hx_clog_format.c` | Pattern engine (`%Y %m %d %H %M %S %e %l %c %t %p %s %# %! %v %x %n %%`) and JSON formatter |
| `src/hx_clog_sink.c` | Sink dispatch + console, callback, syslog, Event Log, logcat and Apple system sinks |
| `src/hx_clog_file.c` | Buffered file sink, reopen |
| `src/hx_clog_file.h` | File sink state shared with rotation |
| `src/hx_clog_rotate.c` | Size/day/interval/startup rotation, archive naming, cleanup by age, compression of older backups beyond the plain-file count |
| `src/hx_clog_async.c` | Bounded ring queue + background worker (built when `HX_CLOG_ENABLE_ASYNC`) |
| `src/hx_clog_crash.c` | SEH / POSIX-signal crash capture (built when `HX_CLOG_ENABLE_CRASH`) |

## Write path

```
HX_LOG_INFO(...)
  -> level filter (atomic load, before any work)
  -> vsnprintf user message (stack buffer, heap fallback for long lines)
  -> capture logger name + thread-local context
  -> custom formatter, JSON formatter, or hx_format_record() assembles the line
  -> push into crash ring buffer (cheap, fixed memory)
  -> SYNC : write to every sink under the sink lock
     ASYNC: copy into the queue; worker drains in batches
  -> FATAL: immediate flush
```

## Threading model

* A single process-wide logger (singleton) guarded by a `once` initializer.
* Level is an atomic int (relaxed) for a cheap hot path.
* Sync writes are serialized by one mutex; the file sink also has its own
  mutex so rotation is safe.
* Async mode: business threads only copy into a pre-allocated ring queue;
  one worker thread writes to sinks and flushes on an interval.

## Platform abstraction

`hx_clog_internal.h` declares thin wrappers (`hx_mutex_*`, `hx_cond_*`,
`hx_thread_*`, time, pid/tid, filesystem) implemented in `hx_clog_core.c`
with `#if` branches for Windows (`CRITICAL_SECTION`, `CONDITION_VARIABLE`,
`_beginthreadex`, `GetSystemTimeAsFileTime`) and POSIX (`pthread`,
`gettimeofday`, `nanosleep`).

## Memory

* Short messages use a 1 KB stack buffer; longer ones fall back to a single
  heap allocation, capped at `HX_CLOG_MAX_LINE`.
* The async queue and the crash ring buffer are pre-allocated at init.
* A user allocator can be installed before init via `hx_clog_set_allocator`.
