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
4. **`hx_clog_async.c`** (optional) is a bounded pre-allocated ring queue + one background worker that drains in batches; business threads only copy into the queue. **`hx_clog_crash.c`** (optional) installs SEH (Windows, `dbghelp`/`StackWalk64`/`MiniDumpWriteDump`) or POSIX signal handlers (`sigaction` + `backtrace`).

`src/hx_clog_internal.h` is the shared internal contract between all `.c` files. `src/hx_clog_file.h` is shared only between the file sink and rotation.

### Sink dispatch detail

The generic `hx_sink_write(sink, level, data, size)` switches on `sink->kind` (`HX_SINK_KIND_CONSOLE/FILE/CALLBACK`) so the level reaches the console sink (for color/stderr routing) and the callback sink without widening the vtable. When adding a sink type, set `kind` in its create function and handle it in this switch.

### Compile gating

`hx_clog_async.c` / `hx_clog_crash.c` are conditionally added to the source list in CMake **and** wrapped in `#if defined(HX_CLOG_ENABLE_ASYNC/CRASH)`. The public crash API symbols always exist: when crash is off, `hx_clog_core.c` provides stub implementations so the ABI stays stable. Anything you add that's optional must keep this dual gating (CMake list + `#if`) and preserve the public ABI.

## Conventions that matter

- **Callback sinks must be added *after* `hx_clog_init()`** — init rebuilds the sink list and discards anything added before it. The tests rely on this ordering.
- **`log_dir` is recursively created** by init (`hx_mkdir_p`) and may be multi-level; **`file_name` must be a plain filename** (subdirectories in `file_name` are *not* created).
- **All files are UTF-8 without BOM.** MSVC builds pass `/utf-8` globally (set right after `project()` in the top-level CMake). Keep files UTF-8 no-BOM; do not let an editor write a BOM or save as UTF-16/GBK.
- Tests are driven through the **public API** (a capture callback sink), not internal symbols, so they pass for both static and shared builds. Follow that pattern for new tests.
- C core targets C99; the wrapper targets C++11. Keep the hot path allocation-free (stack buffer with a single heap fallback for long lines) and keep level filtering ahead of all work.
