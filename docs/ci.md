# CI/CD

[English](ci.md) | [中文](zh-CN/ci.md)

This project uses GitHub Actions to build and test `hx_clog` on every push and
pull request targeting `master`.

## Workflow

The workflow lives at `.github/workflows/ci.yml` and runs:

* Debug and Release builds on Ubuntu, Windows, and macOS.
* Examples and tests with `HX_CLOG_BUILD_EXAMPLES=ON` and
  `HX_CLOG_BUILD_TESTS=ON`.
* `ctest --output-on-failure` for the full test suite.
* A native **linux-arm** (ubuntu-24.04-arm, aarch64) build that runs the full
  test suite on real ARM hardware — this is what catches architecture-specific
  bugs such as wrong raw syscall numbers.
* **Sanitizer** builds: one ASan+UBSan job and one TSan job, both running the
  full test suite, to catch memory errors and data races (e.g. shutdown races
  in the async engine).
* An **options matrix**: `HX_CLOG_ENABLE_ASYNC=OFF`, `HX_CLOG_ENABLE_CRASH=OFF`,
  the combination with `HX_CLOG_ENABLE_ZLIB=OFF`, and a shared-library build —
  verifying the dual compile gating and the stable public ABI stubs.
* A Linux install/package smoke test that verifies the exported CMake package
  and installed public headers.
* Android arm64-v8a and iOS arm64 cross-compile smoke builds, **with the crash
  handler enabled** (Android uses `_Unwind_Backtrace`; no `<execinfo.h>`
  dependency).

Linux CI enables `HX_CLOG_ENABLE_SYSLOG=ON` so the syslog sink path is compiled
and linked. Windows CI covers the default minidump/Event Log link dependencies.
macOS CI covers the Apple platform build path.
Ubuntu/macOS builds use zlib when available, which enables `.gz` compression for
old rotated backups beyond `max_backup_files`.

## Local Verification

From the repository root:

```sh
cmake -S . -B build -DHX_CLOG_BUILD_EXAMPLES=ON -DHX_CLOG_BUILD_TESTS=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

For a Release check:

```sh
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

For a Linux syslog-enabled check:

```sh
cmake -S . -B build-syslog -DCMAKE_BUILD_TYPE=Release -DHX_CLOG_ENABLE_SYSLOG=ON
cmake --build build-syslog --parallel
ctest --test-dir build-syslog --output-on-failure
```

### Enabling zlib for `.gz` rotation

`HX_CLOG_ENABLE_ZLIB` is `ON` by default but only takes effect when CMake's
`find_package(ZLIB)` locates a zlib. On Linux/macOS the system package
(`zlib1g-dev` / `brew install zlib`) is found automatically. On Windows there
is usually no system zlib, so point CMake at one. If you have none, build it
from source once:

```sh
git clone --depth 1 --branch v1.3.1 https://github.com/madler/zlib.git .zlib/src
cmake -S .zlib/src -B .zlib/build -G "Visual Studio 17 2022" -A x64 \
      -DCMAKE_INSTALL_PREFIX=.zlib/install
cmake --build .zlib/build --config Debug   --target install
cmake --build .zlib/build --config Release --target install
```

**Static zlib** (no runtime DLL needed — recommended):

```sh
cmake -S . -B build_zlib -G "Visual Studio 17 2022" -A x64 \
      -DHX_CLOG_ENABLE_ZLIB=ON -DZLIB_USE_STATIC_LIBS=ON \
      -DCMAKE_PREFIX_PATH=.zlib/install
cmake --build build_zlib --config Debug --parallel
ctest --test-dir build_zlib -C Debug --output-on-failure
```

**Dynamic zlib** (links the import library; the `zlib*.dll` must be findable
at run time — on PATH or next to the executables, otherwise tests fail with
`0xc0000135`):

```sh
cmake -S . -B build_zlib_dll -G "Visual Studio 17 2022" -A x64 \
      -DHX_CLOG_ENABLE_ZLIB=ON -DCMAKE_PREFIX_PATH=.zlib/install
cmake --build build_zlib_dll --config Debug --parallel
# make the DLL visible, then run the tests (PowerShell):
#   $env:PATH = ".zlib\install\bin;" + $env:PATH
ctest --test-dir build_zlib_dll -C Debug --output-on-failure
```

When zlib is active, the rotation tests leave `*.gz` backups under the test
log directories; rotated backups beyond `max_backup_files` are compressed
instead of deleted.

## Current Test Coverage

The CTest suite covers:

* Pattern formatting, level filtering, basename/full-path output.
* File rotation by size, startup rotation, and interval time rotation.
* zlib compression of old rotated backups when `HX_CLOG_ENABLE_ZLIB` is active.
* Large line handling.
* Crash handler install/configuration paths.
* Async delivery, blocking overflow behavior, and drop-new overflow accounting.
* Named loggers, thread-local context, JSON formatting, custom formatter,
  runtime reconfiguration, sink IDs, sink removal, sink-level filtering, custom
  allocator routing, and invalid file-name validation.
* Per-sink format overrides (pattern and JSON), duplicate suppression
  ("last message repeated N times"), and the internal error handler.
* UTF-8 (non-ASCII) log directory and file name handling, including rotation,
  via `test_utf8path` — exercises the Windows wide-path layer.
* C++11 RAII wrapper and lightweight `{}` formatting helpers.
