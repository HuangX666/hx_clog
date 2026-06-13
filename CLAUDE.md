# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`hx_clog` is a portable C/C++11 logging framework. The public surface is a **C ABI** (`include/hx_clog.h`); the C++11 layer (`include/hx_clog_cpp.hpp`) is a thin RAII wrapper that only calls the C API and exports no C++ ABI. The library core is a process-wide singleton — there is no logger handle passed around.

## Build & test

```bash
# Configure (MSVC on Windows)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug          # or Release

# Linux/macOS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Run the whole test suite
cd build && ctest -C Debug --output-on-failure

# Run a single test (CTest name == target name)
cd build && ctest -C Debug -R test_async --output-on-failure
# or run the binary directly:
./build/tests/Debug/test_async.exe
```

Key CMake options (all `ON` by default except shared/syslog): `HX_CLOG_BUILD_SHARED`, `HX_CLOG_ENABLE_ASYNC`, `HX_CLOG_ENABLE_CRASH`, `HX_CLOG_ENABLE_CPP11`, `HX_CLOG_BUILD_EXAMPLES`, `HX_CLOG_BUILD_TESTS`. Async and crash sources are only compiled when their option is on; the matching `HX_CLOG_ENABLE_ASYNC` / `HX_CLOG_ENABLE_CRASH` compile definitions gate the code (see "compile gating" below).

To exercise the crash report path, configure with `-DHX_CLOG_TEST_TRIGGER_CRASH=ON`, rebuild `test_crash`, and run it — it deliberately faults and writes `crash_*.log` (+ `.dmp` on Windows) under `./test_crash_logs`.

## Architecture

The write path is the spine — trace it through these files:

1. **`hx_clog_core.c`** owns everything: the global singleton state, the atomic level filter (checked *before* any formatting/allocation), the synchronous write path, the sink list, stats, the user-allocator hook, **all platform/threading abstractions** (`hx_mutex_*`, `hx_cond_*`, `hx_thread_*`, time, pid/tid, `hx_mkdir_p`, file ops — each with `#if` Windows/POSIX branches), and the **crash ring buffer** (pre-allocated "last N logs", lives here so it's available even when crash support is compiled out).
2. **`hx_clog_format.c`** turns a `hx_clog_record_t` + pattern string into a line (`%Y %m %d %H %M %S %e %l %t %p %s %# %! %v %n %%`).
3. **`hx_clog_sink.c`** is the sink dispatch layer plus the console (level-routed: WARN+ → stderr, optional ANSI color) and callback sinks. **`hx_clog_file.c`** is the file sink; **`hx_clog_rotate.c`** does size/daily rotation + backup cleanup and shares state via `hx_clog_file.h` (`struct hx_file_sink_impl`).
4. **`hx_clog_async.c`** (optional) is a bounded ring queue + one background worker that drains in batches; business threads only copy into the queue. Each queue slot owns a heap buffer that **grows on demand and is reused** (steady state allocation-free; buffers above 64 KB are shrunk back on reuse). Slots carry a `target` sink id (0 = all non-override sinks) so per-sink format variants flow through the same queue. The worker copies each line into a single reused scratch buffer under the lock, then releases the lock for the sink IO. **The queue mutex/conds are created once and never destroyed** — destroying them raced with producers during shutdown; `running`/`stop` are only read under the lock. **`hx_clog_crash.c`** (optional) installs SEH (Windows, `dbghelp`/`StackWalk64`/`MiniDumpWriteDump`) or POSIX signal handlers (`sigaction` + `SA_ONSTACK`/`sigaltstack`; `backtrace()` on glibc/Apple, `_Unwind_Backtrace` on Android/musl). The POSIX handler is async-signal-safe by policy: `dladdr`-only symbolization (module+offset, resolved offline), no popen/malloc/stdio; the Windows filter must never call `hx_clog_flush()`.

`src/hx_clog_internal.h` is the shared internal contract between all `.c` files. `src/hx_clog_file.h` is shared only between the file sink and rotation.

### Sink dispatch detail

The generic `hx_sink_write(sink, level, data, size)` switches on `sink->kind` (`HX_SINK_KIND_CONSOLE/FILE/CALLBACK`) so the level reaches the console sink (for color/stderr routing) and the callback sink without widening the vtable. When adding a sink type, set `kind` in its create function and handle it in this switch. System sinks (syslog/event log/logcat/os_log) are level-routed through `system_sink_emit` and have `vtable->write == NULL` on purpose — do not add a level-less fallback write.

### Format settings and per-sink overrides

Global pattern/format-mode/formatter live in core guarded by `sink_lock`, but the hot path reads them from a **per-thread cache** refreshed only when `format_gen` changes — never add a per-log lock acquisition for format settings. Per-sink overrides (`hx_clog_set_sink_pattern` / `hx_clog_set_sink_format_mode`) are stored on the sink and counted in the atomic `override_count`; `core_dispatch_record()` renders one default line plus one line per override sink and routes them via the `target` sink id (sync and async).

### Compile gating

`hx_clog_async.c` / `hx_clog_crash.c` are conditionally added to the source list in CMake **and** wrapped in `#if defined(HX_CLOG_ENABLE_ASYNC/CRASH)`. The public crash API symbols always exist: when crash is off, `hx_clog_core.c` provides stub implementations so the ABI stays stable. Anything you add that's optional must keep this dual gating (CMake list + `#if`) and preserve the public ABI.

## Conventions that matter

- **Callback sinks must be added *after* `hx_clog_init()`** — init rebuilds the sink list and discards anything added before it. The tests rely on this ordering.
- **`log_dir` is recursively created** by init (`hx_mkdir_p`) and may be multi-level; **`file_name` must be a plain filename** (subdirectories in `file_name` are *not* created).
- **All files are UTF-8 without BOM.** MSVC builds pass `/utf-8` globally (set right after `project()` in the top-level CMake). Keep files UTF-8 no-BOM; do not let an editor write a BOM or save as UTF-16/GBK.
- **All paths are UTF-8 at runtime too.** On Windows every filesystem touch must go through the wide-API helpers in core (`hx_fopen`, `hx_file_exists`, `hx_rename`, `hx_remove`, `hx_mkdir_p`, `hx_utf8_to_wide`/`hx_wide_to_utf8`) — never call `fopen`/`*A()` Win32 functions on a path directly. `test_utf8path` enforces this with a Chinese dir/file name.
- **`hx_clog_config_t` grows at the end only** (1.1.0: `rotate_align`, `max_compressed_files`; 1.2.0: `date_subdir`). Callers must initialize via `hx_clog_config_default()`; never insert fields in the middle. Retention defaults are `-1` (= unlimited / never delete) for both `max_backup_files` and `max_compressed_files`; cleanup orders backups by the date+index parsed from the name, not by mtime. When `date_subdir` is on the file sink writes under `<log_dir>/<YYYY-MM-DD>/` and rolls to a new folder at the day boundary; rotation/cleanup operate on `fs->active_dir` (the dated dir), not `fs->dir`.
- Internal failures are reported through `hx_core_report_error()` (user-installed `hx_clog_set_error_handler`) — never printf from inside the library, and never call the handler while holding a lock the handler's contract doesn't know about.
- Tests are driven through the **public API** (a capture callback sink), not internal symbols, so they pass for both static and shared builds. Follow that pattern for new tests.
- C core targets C99; the wrapper targets C++11. Keep the hot path allocation-free (stack buffer with a single heap fallback for long lines) and keep level filtering ahead of all work.
- **Per-line size cap**: a single line is capped at 512 KB by default in *both* sync and async modes, enforced via `hx_clamp_line()` in `hx_clog_internal.h`. CMake `HX_CLOG_UNLIMITED_LINE=ON` removes the cap (memory-bound); `-DHX_CLOG_MAX_LINE_BYTES=N` overrides it. `test_largeline` covers both capped and unlimited behavior. The crash ring buffer entries stay fixed at `HX_CLOG_RING_ENTRY_SIZE` (512 B) by design — that only limits the crash-report replay, not normal output.
