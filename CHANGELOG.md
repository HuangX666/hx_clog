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
- `hx_clog_crash_handler_recheck()`: re-asserts the crash handler if another
  component replaced the process-wide exception filter / signal handlers
  (returns 1 when it re-installed, 0 when intact).
- MinGW CI job (MSYS2/mingw64, build + ctest), closing the long-standing gap
  where the Windows-without-MSVC code paths had no CI coverage.

### Fixed
- Crash callback publication (`hx_clog_set_crash_callback`) now uses full
  barriers on both sides, so a concurrent crash can no longer observe a new
  callback paired with the previous user data (audit P2-13).
- **Network sink latency bounding (audit P0-1 / P1-2 / P1-3)**: TCP sockets
  carry `SO_SNDTIMEO`, so a collector that stops reading drops the link
  within 2 s instead of freezing every logging thread holding the sink lock;
  DNS is resolved once at sink creation (outside any lock) and cached —
  steady-state reconnects never re-enter the resolver; `select()`/`FD_SET`
  replaced with `poll()`/`WSAPoll()`; the reconnect backoff window uses a
  monotonic clock (NTP steps no longer disable the sink); `EINTR` retries
  instead of tearing the link down.
- **INI config hardening (audit P1-4 / P2-11)**: numeric keys go through a
  validating parser — negative and garbage values are rejected (default
  kept, reported), oversized values are clamped into range, size suffixes
  saturate instead of wrapping (a negative `async_queue_size` used to become
  ~4 billion and could wrap the slot allocation); lines longer than the
  1200-byte reader buffer are dropped whole instead of being applied as a
  truncated fragment plus a bogus continuation line.
- **Shutdown / teardown (audit P1-5)**: the atexit flush is bounded (1.5 s)
  and worker-liveness-checked (Windows probes the thread handle, so a worker
  killed by `ExitProcess` before DLL_PROCESS_DETACH no longer hangs the
  process under the loader lock).
- **Concurrency / lifecycle (audit P1-1, P2-6, P2-3)**: `hx_clog_init()`
  rebuilds the sink table and format fields under `sink_lock` (matching every
  other writer) and properly closes sinks that were added before init instead
  of leaking them (including the network sink's WSAStartup reference);
  `hx_clog_reconfigure()` keeps network sinks alive alongside callback sinks
  instead of silently ending remote logging.
- **Logger registry (audit P2-7 / P2-8)**: names longer than the fixed
  127-byte storage are rejected instead of being registered truncated (which
  made every `logger_get` of a long name allocate a fresh entry forever);
  `hx_clog_logger_drop_all()` detaches entries without freeing them, so
  pointers held by concurrent writers stay valid.
- `hx_clog_get_config()` refreshes `level`/`mode` live (a later
  `hx_clog_set_level` is reflected) and internal string copies are made
  alias-safe for the documented get→modify→reconfigure loop (audit P2-9).
- Ring-buffer allocation failure is reported outside `init_lock`, so an
  error handler that calls back into the library cannot deadlock (audit
  P2-10).
- **Robustness details**: `HX_LOG_*_EVERY_N(0)` logs every call instead of
  dividing by zero, and the per-site counter is now an atomic increment
  (audit P2-1); UDP lines over the datagram limit are dropped per-line
  without tearing the connectionless link down (audit P2-2); sockets are
  created non-inheritable and forked children drop inherited TCP connections
  instead of interleaving on one stream (audit P2-4); the JSON formatter
  validates UTF-8 and replaces damaged bytes with U+FFFD so every emitted
  line is valid JSON (audit P2-12); the console sink reports persistently
  failing writes instead of silently swallowing them; the C11 atomics
  fallback uses a genuine `_Atomic int` typedef instead of a cast (audit
  P2-14).
- Header version macros corrected to 1.3.0 (were 1.2.0; audit P1-6).

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
