# hx_clog Crash 处理

[English](../crash.md) | [中文](crash.md)

Crash 捕获采用保守策略：发生 fatal signal 或 unhandled exception 后，进程可能已经处于受损状态，因此 handler 只执行低层工作，并使用 raw file I/O 写出报告。

## 启用方式

使用 `HX_CLOG_ENABLE_CRASH=ON`（默认）构建，然后可以通过主配置启用：

```c
hx_clog_config_t cfg;
hx_clog_config_default(&cfg);
cfg.enable_crash_handler = 1;   /* installs default crash config during init */
hx_clog_init(&cfg);
```

也可以显式安装，以获得更细粒度的控制：

```c
hx_clog_crash_config_t cc;
hx_clog_crash_config_default(&cc);
cc.crash_dir = "./logs";
cc.create_minidump = 1;          /* Windows .dmp */
cc.dump_registers = 1;           /* optional CPU register dump */
cc.symbolize_stacktrace = 1;
hx_clog_install_crash_handler(&cc);
```

## 报告内容

```text
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

* **Windows** 使用 `SetUnhandledExceptionFilter`、`StackWalk64` 和 `dbghelp`（`SymFromAddr` / `SymGetLineFromAddr64`）进行符号化，并可选使用 `MiniDumpWriteDump` 生成 `.dmp`。符号化需要 PDB。Filter 故意不调用 `hx_clog_flush()`，因为崩溃线程可能正持有 sink lock，从而导致死锁；`last_logs` ring buffer 会携带最近日志。
* **POSIX** 为 `SIGSEGV`、`SIGABRT`、`SIGFPE`、`SIGILL`、`SIGBUS` 安装 `sigaction` handler，并在专用 `sigaltstack` 上使用 `SA_ONSTACK`，因此栈溢出的 `SIGSEGV` 也能生成报告。glibc/Apple 使用 `backtrace()`，Android/musl 使用 `_Unwind_Backtrace` 捕获帧，所以 crash handler 也能在移动端构建并工作。符号化只使用 `dladdr`，也就是模块路径、偏移和符号名；不在进程内使用 `popen`/`addr2line`，因为在 signal handler 中 fork 或分配内存可能在堆损坏时死锁。文件/行号请离线解析：

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

在报告主体写入后触发，参数包含打开的报告 fd，应用可以追加自己的上下文，例如 session id、build id 等。该 callback 必须只执行 async-signal-safe 操作。

## 生成良好栈追踪的编译选项

| 编译器 | 选项 |
| --- | --- |
| MSVC | `/Zi` 或 `/Z7`，链接 `/DEBUG`，由 CMake 设置 |
| GCC / Clang | `-g -fno-omit-frame-pointer`，frame pointer 由 CMake 选项保留 |

GCC/Clang 下会添加 `-rdynamic`，以便非 static 符号能在栈追踪中解析。

## Last-N log ring buffer

每条日志都会被复制进固定、预分配的 ring buffer（`HX_CLOG_RING_CAPACITY` 个 entry）。Crash 时 handler 使用简单 `write()` 调用输出它，不使用 `malloc`，并且 crash 路径不会主动获取普通锁，因此最近的业务日志能进入报告。

## 试运行

使用 `-DHX_CLOG_TEST_TRIGGER_CRASH=ON` 配置，重新构建 `test_crash` 并运行它。测试会解引用空指针，并在 `./test_crash_logs` 下留下 `crash_*.log`，Windows 上还会生成 `crash_*.dmp`。

