# CI/CD

This project uses GitHub Actions to build and test `hx_clog` on every push and
pull request targeting `master`.

## Workflow

The workflow lives at `.github/workflows/ci.yml` and runs:

* Debug and Release builds on Ubuntu, Windows, and macOS.
* Examples and tests with `HX_CLOG_BUILD_EXAMPLES=ON` and
  `HX_CLOG_BUILD_TESTS=ON`.
* `ctest --output-on-failure` for the full test suite.
* A Linux install/package smoke test that verifies the exported CMake package
  and installed public headers.
* Android arm64-v8a and iOS arm64 cross-compile smoke builds.

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
* C++11 RAII wrapper and lightweight `{}` formatting helpers.
