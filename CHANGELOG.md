# Changelog

All notable changes to hx_clog are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/) and the project aims to follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- **Windows crash-handler hardening** (partial mitigation for the "memory
  corruption produces no dump" class of failures):
  - Re-entrancy guard in the SEH filter: a nested fault while writing the
    report no longer recurses.
  - Static artifact name buffers, so `EXCEPTION_STACK_OVERFLOW` can still
    write a report without ~3 KB of path buffers on the exhausted stack.
  - dbghelp (`SymInitialize` + a throwaway stack walk) is warmed up at
    install time instead of running for the first time inside the exception
    filter.
  - `abort()`/SIGABRT, CRT invalid parameters and C++ pure virtual calls are
    funnelled into the filter as custom exceptions — they terminate without
    SEH dispatch and previously produced neither report nor dump.
  - Crash artifacts use UTC timestamps computed without `localtime` (whose
    MinGW path takes a lock the crashing thread may already hold), unique
    `crash_<date>_<time>_pid<pid>_<seq>` names (same-second crashes no longer
    overwrite each other), and minidump failures are recorded in the report.
  - After writing its artifacts the filter hands the exception to Windows
    Error Reporting (`EXCEPTION_CONTINUE_SEARCH`) so an out-of-process dump
    path stays alive; disable with the new `WER_PASSTHROUGH` option.
  - New APIs: `hx_clog_crash_set_option()` (`EXTRA_HANDLERS` /
    `WER_PASSTHROUGH` / `MINIDUMP_TYPE` with a large "corruption-analysis"
    preset) and `hx_clog_wer_local_dumps_enable()` / `_disable()` — opt-in
    per-app WER LocalDumps registration under HKCU, the only reliable dump
    source for fail-fast terminations (heap metadata corruption, `/GS` stack
    cookie) that no in-process handler can observe. The registration creates
    the dump folder up front and forces a hive flush (`RegFlushKey`) so it
    survives an immediate power loss; a partially-written registration
    degrades to WER's documented defaults instead of failing.
- Detailed copyright and per-file purpose headers on every source file
  (library files describe their responsibilities and locking/ABI contracts;
  tests and examples carry a compact copyright line).

### Fixed
- Crash callback publication (`hx_clog_set_crash_callback`) now uses full
  barriers on both sides, so a concurrent crash can no longer observe a new
  callback paired with the previous user data (audit P2-13).

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
