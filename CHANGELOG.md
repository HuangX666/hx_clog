# Changelog

All notable changes to hx_clog are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/) and the project aims to follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

## [1.3.0] - 2026-06-20

### Added
- **Compile-time level cutting**: `HX_CLOG_ACTIVE_LEVEL` (with the
  `HX_CLOG_LEVEL_NUM_*` constants). Macros below the active level expand to
  `((void)0)` — no code emitted, arguments not evaluated (spdlog-style).
- **Conditional logging macros**: `HX_LOG_<LEVEL>_IF(cond, ...)` and
  `HX_LOG_<LEVEL>_EVERY_N(n, ...)` (glog-style).
- **TCP/UDP network sink**: `hx_clog_add_network_sink()` (gated by
  `HX_CLOG_ENABLE_NET`, default on). Lazy connect, bounded TCP connect with
  rate-limited reconnect, UDP fire-and-forget.
- **INI config file**: `hx_clog_init_from_file()` (`[hx_clog]` section,
  `key=value`, `K`/`M`/`G` size suffixes, `#`/`;` comments).
- **Named-logger registry + dotted hierarchy**: `hx_clog_logger_get` /
  `_find` / `_count` / `_drop_all`, and `hx_clog_set_level_for_prefix`
  (a new `a.b.c` inherits its nearest registered ancestor's level).
- **Config read-back**: `hx_clog_get_config()`.
- **atexit flush**: buffered/async lines are flushed on normal process exit.
- C11 `_Thread_local` / `<stdatomic.h>` fallbacks for compilers that are
  neither MSVC nor GCC/Clang.

### Fixed
- Rotation cleanup no longer deletes non-rotation files that merely share the
  active log's stem/extension (strict `<stem>.YYYY-MM-DD.<index>[.ext][.gz]`
  matching); candidate set grows past 512.
- Async `reconfigure()` is lossless under concurrent logging (no silent,
  uncounted drops with `HX_CLOG_OVERFLOW_BLOCK`).
- `hx_clog_get_stats()` before init no longer faults.
- POSIX after-fork no longer re-initializes possibly-held mutexes (added
  `pthread_atfork` handlers); recursive `sink_lock` so callbacks may re-enter
  the API; data race on the default logger name and on async stat counters
  fixed (TSan-clean).
- Repeated `%v`/`%x` patterns expand fully (exact heap sizing) instead of
  truncating below the line cap.
- MinGW: request C99 `vsnprintf` (`__USE_MINGW_ANSI_STDIO`) so long messages
  are not silently dropped; non-thread-safe `localtime()` serialized.
- Crash-handler install checks `sigaltstack`/`sigaction` results; OOM in the
  crash ring and rotation cleanup is reported via the error handler.
- `-latomic` is linked / added to `Libs.private` on targets that need it.

### Notes
- New public symbols are append-only; the C ABI remains stable. Optional
  features keep their stub symbols when compiled out.
