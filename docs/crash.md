# hx_clog Crash Handling

[English](crash.md) | [中文](zh-CN/crash.md)

Crash capture is conservative: after a fatal signal / unhandled exception the
process may be in a damaged state, so the handler only does low-level work and
writes a report using raw file I/O.

## Enabling

Build with `HX_CLOG_ENABLE_CRASH=ON` (default), then either:

```c
hx_clog_config_t cfg;
hx_clog_config_default(&cfg);
cfg.enable_crash_handler = 1;   /* installs default crash config during init */
hx_clog_init(&cfg);
```

or install it explicitly for finer control:

```c
hx_clog_crash_config_t cc;
hx_clog_crash_config_default(&cc);
cc.crash_dir = "./logs";
cc.create_minidump = 1;          /* Windows .dmp */
cc.dump_registers = 1;           /* optional CPU register dump */
cc.symbolize_stacktrace = 1;
hx_clog_install_crash_handler(&cc);
```

## What a report contains

```
========== hx_clog crash report ==========
time: 2026-06-07 21:05:12.427
pid: 275828
thread: 283676

exception:
  type: EXCEPTION_ACCESS_VIOLATION
  code: 0x00000000c0000005
  instruction_pointer: 0x00007ff7f04c194d
  fault_address: 0x0000000000000000

stacktrace:
  #00 0x00007ff7f04c194d main
      D:\...\tests\test_crash.c:48
  ...

registers:
  RIP: 0x00007ff7f04c194d
  RSP: 0x00000000...

last_logs:
  2026-06-07 21:05:12.426 [INFO ] [tid:283676] ... recent log line 2
==========================================
```

* **Windows** uses `SetUnhandledExceptionFilter`, `StackWalk64` and
  `dbghelp` (`SymFromAddr` / `SymGetLineFromAddr64`) for symbolization, and
  optionally `MiniDumpWriteDump` for a `.dmp`. Symbolization needs PDBs.
  The filter deliberately does **not** call `hx_clog_flush()` — the crashing
  thread may hold the sink lock and would deadlock; the `last_logs` ring
  buffer section carries the recent lines instead.
* **POSIX** installs `sigaction` handlers (with `SA_ONSTACK` on a dedicated
  `sigaltstack`, so stack-overflow `SIGSEGV` still produces a report) for
  `SIGSEGV`, `SIGABRT`, `SIGFPE`, `SIGILL`, `SIGBUS`. Frames are captured with
  `backtrace()` on glibc/Apple and `_Unwind_Backtrace` on Android/musl (so the
  crash handler builds and works on mobile too). Symbolization uses `dladdr`
  only — module path + offset + symbol name, **no** `popen`/`addr2line`
  in-process (forking and allocating inside a signal handler deadlocks exactly
  when the heap is corrupted). Resolve file/line offline:

  ```bash
  addr2line -f -p -e <module> <module_offset>   # Linux
  atos -o <module> -l <load_address> <address>   # macOS
  ```

### Crash callback (1.1.0)

```c
void my_crash_cb(int fd, void* ud) {
    /* async-signal-safe only: write(fd, ...) */
}
hx_clog_set_crash_callback(my_crash_cb, NULL);
```

Invoked after the report body is written, with the open report fd, so the app
can append its own context (session id, build id, ...).

## Build flags for good stack traces

| Compiler | Flags |
| --- | --- |
| MSVC | `/Zi` (or `/Z7`), link `/DEBUG` (set by CMake) |
| GCC / Clang | `-g -fno-omit-frame-pointer` (frame pointer kept by CMake option) |

`-rdynamic` is added on GCC/Clang so non-static symbols resolve in traces.

## Last-N log ring buffer

Every log line is copied into a fixed, pre-allocated ring buffer
(`HX_CLOG_RING_CAPACITY` entries). On crash the handler dumps it with simple
`write()` calls — no `malloc`, no locks taken on the crash path beyond what is
unavoidable — so the most recent business logs survive into the report.

## Trying it

Configure with `-DHX_CLOG_TEST_TRIGGER_CRASH=ON`, rebuild `test_crash`, and run
it: it dereferences a null pointer and leaves a `crash_*.log` (and on Windows a
`crash_*.dmp`) under `./test_crash_logs`.
