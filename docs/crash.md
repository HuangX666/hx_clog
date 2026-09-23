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
time: 2026-09-01 15:10:55.840 UTC          <- Windows reports use UTC (see below)
pid: 22284
thread: 29596

exception:
  type: EXCEPTION_ACCESS_VIOLATION
  code: 0x00000000c0000005
  instruction_pointer: 0x00007ff7460913ee
  fault_address: 0x0000000000000000

stacktrace:
  #00 0x00007ff7460913ee main
  ...

minidump: ./test_crash_logs/crash_..._pid22284_1.dmp   <- on failure: the error instead

last_logs:
  2026-09-01 23:10:55.832 [INFO ] [tid:29596] ... recent log line 2
==========================================
```

Artifact names are `crash_<date>_<time>_pid<pid>_<seq>.log` / `.dmp` — the pid
and sequence number keep two crashes within the same second from silently
overwriting each other.

* **Windows** uses `SetUnhandledExceptionFilter`, `StackWalk64` and
  `dbghelp` (`SymFromAddr` / `SymGetLineFromAddr64`) for symbolization, and
  optionally `MiniDumpWriteDump` for a `.dmp`. Symbolization needs PDBs.
  The filter deliberately does **not** call `hx_clog_flush()` — the crashing
  thread may hold the sink lock and would deadlock; the `last_logs` ring
  buffer section carries the recent lines instead. The filter is additionally
  hardened for the "heap already corrupted" case:
  - a re-entrancy guard stops a nested fault inside the filter from recursing;
  - the artifact name buffers are static, so `EXCEPTION_STACK_OVERFLOW` can
    still write a report without ~3 KB of path buffers on the exhausted stack;
  - dbghelp is initialized (and exercised once) at install time, not for the
    first time inside the filter;
  - report timestamps are computed as **UTC** without `localtime` (whose
    MinGW path takes a lock the crashing thread may already hold);
  - `abort()` / SIGABRT, CRT invalid parameters and C++ pure virtual calls are
    funnelled into the filter as custom exceptions — they otherwise terminate
    without any SEH dispatch and would leave neither report nor dump;
  - minidump failures are recorded inside the `.log` instead of vanishing;
  - after writing its artifacts the filter hands the exception to Windows
    Error Reporting (see below), keeping an out-of-process dump path alive.
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

## Windows: which terminations produce a dump

| Termination path | In-process report/dump | Out-of-process (WER LocalDumps) |
| --- | --- | --- |
| Unhandled SEH exception (access violation, ...) | ✅ | ✅ |
| `abort()` / SIGABRT | ✅ (funnelled since 1.4.0) | ✅ |
| CRT invalid parameter (retail UCRT) | ✅ (funnelled since 1.4.0) | ✅ |
| C++ pure virtual call | ✅ (funnelled since 1.4.0, MSVC) | ✅ |
| `MiniDumpWriteDump` fails on corrupted heap | ❌ (but the failure is logged in the report) | ✅ |
| Fail-fast: heap metadata corruption, /GS stack cookie | ❌ impossible in-process | ✅ |
| `TerminateProcess` / `ExitProcess` / hang | ❌ | ❌ (needs a watchdog) |

Fail-fast terminations (`__fastfail`) bypass SEH entirely — **no in-process
handler, ours or anyone else's, can observe them**. The only reliable dump
source for them is Windows Error Reporting, which runs out of process.
Register that safety net with the opt-in helper:

```c
/* per-app LocalDumps under
 * HKCU\Software\Microsoft\Windows\Windows Error Reporting\LocalDumps\<exe>
 * (HKCU: no administrator rights). WER then writes <exe>.<pid>.dmp for every
 * WER-reported termination, including fail-fast ones. */
hx_clog_wer_local_dumps_enable(NULL /* = crash_dir */, 1 /* minidump */, 10);
```

Notes:
- `WER_PASSTHROUGH` (default on, see runtime options below) is required for
  this to fire — after writing its own artifacts the filter returns the
  exception to WER instead of terminating directly.
- WER must not be disabled by policy. If the host process sets
  `SEM_NOGPFAULTERRORBOX` (some shells/schedulers do), WER is suppressed;
  the error mode is inherited by children, so apps launched from such hosts
  should reset it with `SetErrorMode(0)`.
- Per-app `DumpFolder` may be ignored when a machine-level WER policy exists;
  dumps then land in the default `%LOCALAPPDATA%\CrashDumps`.

### Registration durability (reboot / power loss / corrupted memory)

The LocalDumps registration lives in the user's profile hive (`NTUSER.DAT`)
and survives reboots. `enable()` additionally hardens the adverse scenarios:

- **Power loss right after enable()** — the call forces a hive flush
  (`RegFlushKey`) before returning. `RegCloseKey` alone does not guarantee
  on-disk persistence (the kernel flushes hives lazily), so without this the
  registration could be lost even though the call returned OK.
- **Crash while enable() is running / heap already corrupted** — registry
  values are written by the kernel's configuration manager; in-process
  memory corruption cannot corrupt the hive. The worst case is a plain write
  failure, which is returned (and reported through the error handler).
- **Only part of the three values got written** — values are stored
  most-important-first and WER substitutes its documented defaults for
  missing ones (`DumpType` → minidump, `DumpCount` → 10), so a partial
  registration still produces dumps, in the configured folder.
- **Dump folder missing at crash time** — closed up front: `enable()`
  creates the (absolute) folder itself, since WER may silently skip a
  missing folder.
- **Machine hard-power-loss during the crash itself** — the `.dmp` WER is
  writing can be truncated. Inherent to any crash-time artifact; unrelated
  to the registration.

Residual scope caveats (how LocalDumps works, not fixable in process):

- `HKCU` is per-user: a Windows service running as SYSTEM / another account
  needs its own registration under that account, or a machine-wide `HKLM`
  registration (requires elevation — commonly done by deployment tooling).
- The per-application key is matched by executable file NAME only: two
  different applications whose exes share a name share one registration.
- Registry cleaners or profile resets can delete the key. Re-calling
  `enable()` at every startup is idempotent, cheap, and self-healing —
  recommended.

### Crash callback (1.1.0)

```c
void my_crash_cb(int fd, void* ud) {
    /* async-signal-safe only: write(fd, ...) */
}
hx_clog_set_crash_callback(my_crash_cb, NULL);
```

Invoked after the report body is written, with the open report fd, so the app
can append its own context (session id, build id, ...).

### Runtime options (1.4.0)

```c
hx_clog_crash_set_option(HX_CLOG_CRASH_OPT_EXTRA_HANDLERS,  1); /* default */
hx_clog_crash_set_option(HX_CLOG_CRASH_OPT_WER_PASSTHROUGH, 1); /* default */
hx_clog_crash_set_option(HX_CLOG_CRASH_OPT_MINIDUMP_TYPE,   2);
```

* `EXTRA_HANDLERS` — capture `abort()` / CRT invalid parameters / pure virtual
  calls (see the table above). Toggling installs/removes the underlying CRT
  handlers immediately; default on.
* `WER_PASSTHROUGH` — after writing the report and minidump, hand the
  exception to Windows Error Reporting instead of terminating directly;
  default on. Side effect: the system "program stopped working" flow may
  become visible.
* `MINIDUMP_TYPE` — `0` minimal, `1` default (indirectly referenced memory),
  `2` large (adds data segments, handles, thread info — the most useful level
  for memory-corruption analysis), `3` full memory.

On POSIX the options are accepted and ignored (POSIX already catches SIGABRT
and re-raises after writing its report).

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

Configure with `-DHX_CLOG_TEST_TRIGGER_CRASH=ON`, rebuild `test_crash` (use a
Release build — the debug CRT asserts instead of reporting invalid
parameters), and run it with a mode argument: `null` (default,
null-pointer dereference), `abort` (`abort()`), or `invalid` (CRT invalid
parameter, e.g. `fclose(NULL)`). Each terminates the process by design and
leaves a `crash_*_pid<N>_<seq>.log` (and on Windows a matching `.dmp`) under
`./test_crash_logs`. The trigger build disables WER pass-through so the run
stays deterministic (no system dialog).
