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
time: 2026-09-01 15:10:55.840 UTC          <- Windows 报告使用 UTC(见下文)
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

minidump: ./test_crash_logs/crash_..._pid22284_1.dmp   <- 失败时此处记录错误码

last_logs:
  2026-09-01 23:10:55.832 [INFO ] [tid:29596] ... recent log line 2
==========================================
```

产物命名为 `crash_<日期>_<时间>_pid<pid>_<序号>.log` / `.dmp`——pid 与自增序号保证同一秒内的两次崩溃不会互相覆盖。

* **Windows** 使用 `SetUnhandledExceptionFilter`、`StackWalk64` 和 `dbghelp`（`SymFromAddr` / `SymGetLineFromAddr64`）进行符号化，并可选使用 `MiniDumpWriteDump` 生成 `.dmp`。符号化需要 PDB。Filter 故意不调用 `hx_clog_flush()`，因为崩溃线程可能正持有 sink lock，从而导致死锁；`last_logs` ring buffer 会携带最近日志。Filter 针对"堆已损坏"场景做了强化：
  - 重入保护：filter 内部再次出错时直接放行，不再递归；
  - 产物名缓冲为静态存储，`EXCEPTION_STACK_OVERFLOW` 时不需要在耗尽的栈上放 ~3 KB 路径缓冲，仍可写出报告；
  - dbghelp 在安装时初始化并预热一次，而不是在异常过滤器内第一次使用；
  - 报告时间戳用**纯数学换算的 UTC**，不经 `localtime`（其 MinGW 路径要拿锁，崩溃线程可能已持有该锁）；
  - `abort()` / SIGABRT、CRT 无效参数、C++ 纯虚调用被以自定义异常汇入 filter——它们原本不经过任何 SEH 派发，什么都不会留下；
  - minidump 失败会记录在 `.log` 内，而不是静默消失；
  - 写完产物后把异常交还给 Windows 错误报告（WER），保住进程外 dump 通道（见下文）。
* **POSIX** 为 `SIGSEGV`、`SIGABRT`、`SIGFPE`、`SIGILL`、`SIGBUS` 安装 `sigaction` handler，并在专用 `sigaltstack` 上使用 `SA_ONSTACK`，因此栈溢出的 `SIGSEGV` 也能生成报告。glibc/Apple 使用 `backtrace()`，Android/musl 使用 `_Unwind_Backtrace` 捕获帧，所以 crash handler 也能在移动端构建并工作。符号化只使用 `dladdr`，也就是模块路径、偏移和符号名；不在进程内使用 `popen`/`addr2line`，因为在 signal handler 中 fork 或分配内存可能在堆损坏时死锁。文件/行号请离线解析：

  ```bash
  addr2line -f -p -e <module> <module_offset>   # Linux
  atos -o <module> -l <load_address> <address>   # macOS
  ```

## Windows：哪些终止路径能生成 dump

| 终止路径 | 进程内报告/dump | 进程外（WER LocalDumps） |
| --- | --- | --- |
| 未处理 SEH 异常（访问违例等） | ✅ | ✅ |
| `abort()` / SIGABRT | ✅（1.4.0 起汇入） | ✅ |
| CRT 无效参数（release UCRT） | ✅（1.4.0 起汇入） | ✅ |
| C++ 纯虚调用 | ✅（1.4.0 起汇入，MSVC） | ✅ |
| `MiniDumpWriteDump` 在损坏的堆上失败 | ❌（但失败会记录在报告中） | ✅ |
| Fail-fast：堆元数据损坏、/GS 栈 cookie | ❌ 进程内不可能 | ✅ |
| `TerminateProcess` / `ExitProcess` / 挂死 | ❌ | ❌（需要看门狗） |

Fail-fast 类终止（`__fastfail`）完全绕过 SEH——**任何进程内 handler（包括本库）都不可能捕获**。唯一可靠的 dump 来源是在进程外运行的 Windows 错误报告。可按需注册该兜底：

```c
/* 在
 * HKCU\Software\Microsoft\Windows\Windows Error Reporting\LocalDumps\<exe>
 * 下注册按应用 LocalDumps（HKCU 无需管理员权限）。此后 WER 会对每次
 * 上报的终止（含 fail-fast）写出 <exe>.<pid>.dmp。 */
hx_clog_wer_local_dumps_enable(NULL /* = crash_dir */, 1 /* minidump */, 10);
```

注意：
- 需要 `WER_PASSTHROUGH`（默认开，见运行期选项）——filter 写完自己的产物后把异常交还 WER，而不是直接终止。
- WER 不能被策略禁用。宿主进程若设置了 `SEM_NOGPFAULTERRORBOX`（某些 shell/调度器会设置），WER 会被抑制；error mode 会被子进程继承，从这类宿主启动的应用应自行 `SetErrorMode(0)` 重置。
- 机器级 WER 策略存在时按应用配置的 `DumpFolder` 可能被忽略，dump 会落在默认的 `%LOCALAPPDATA%\CrashDumps`。

### 注册可靠性（重启 / 掉电 / 内存损坏）

LocalDumps 注册保存在用户配置单元（`NTUSER.DAT`）中，重启后依然有效。`enable()` 对下列不利场景做了针对性加固：

- **enable() 之后立即掉电**——返回前强制刷写配置单元（`RegFlushKey`）。仅 `RegCloseKey` 并不保证落盘（内核惰性刷写 hive），不刷写的话调用虽然返回成功，掉电后注册可能丢失。
- **enable() 执行途中进程崩溃 / 堆已损坏**——注册表值由内核配置管理器写入，进程内内存损坏**不可能**破坏 hive；值经拷贝传递，最坏情况是普通写入失败（会返回错误并上报 error handler）。
- **三个值只写入了一部分**——值按重要性先后写入，WER 对缺失值使用文档默认（`DumpType` → minidump、`DumpCount` → 10），部分注册仍能产出 dump，且落在已写入的 `DumpFolder` 里。
- **崩溃时 DumpFolder 目录不存在**——由 `enable()` 提前创建（绝对路径），因为 WER 对缺失目录可能静默跳过。
- **崩溃瞬间机器硬断电**——WER 正在写的 `.dmp` 可能截断。这是任何崩溃时产物的固有限制，与注册表无关。

残余的范围性注意（LocalDumps 机制本身如此，进程内无法解决）：

- `HKCU` 按用户隔离：以 SYSTEM / 其他账户运行的 Windows 服务需要在该账户下另行注册，或用机器级 `HKLM` 注册（需管理员权限，通常由部署工具完成）。
- 按应用键以可执行文件**名**匹配：两个不同应用的 exe 同名时共用一份注册。
- 注册表清理工具 / 配置文件重置可能删除该键。每次启动重调 `enable()` 幂等、廉价且可自愈——推荐。

### Crash callback (1.1.0)

```c
void my_crash_cb(int fd, void* ud) {
    /* async-signal-safe only: write(fd, ...) */
}
hx_clog_set_crash_callback(my_crash_cb, NULL);
```

在报告主体写入后触发，参数包含打开的报告 fd，应用可以追加自己的上下文，例如 session id、build id 等。该 callback 必须只执行 async-signal-safe 操作。

### 运行期选项 (1.4.0)

```c
hx_clog_crash_set_option(HX_CLOG_CRASH_OPT_EXTRA_HANDLERS,  1); /* 默认 */
hx_clog_crash_set_option(HX_CLOG_CRASH_OPT_WER_PASSTHROUGH, 1); /* 默认 */
hx_clog_crash_set_option(HX_CLOG_CRASH_OPT_MINIDUMP_TYPE,   2);
```

* `EXTRA_HANDLERS` —— 捕获 `abort()` / CRT 无效参数 / 纯虚调用（见上表）。切换会立即安装/移除底层 CRT handler；默认开。
* `WER_PASSTHROUGH` —— 写完报告和 minidump 后把异常交给 Windows 错误报告而不是直接终止；默认开。副作用：系统"程序已停止工作"流程可能变得可见。
* `MINIDUMP_TYPE` —— `0` 最小、`1` 默认（间接引用内存）、`2` 大（附加数据段、句柄、线程信息——分析内存损坏最有用的一档）、`3` 全内存。

POSIX 上这些选项被接受但忽略（POSIX 本就捕获 SIGABRT 并在写完报告后重新 raise）。

## 生成良好栈追踪的编译选项

| 编译器 | 选项 |
| --- | --- |
| MSVC | `/Zi` 或 `/Z7`，链接 `/DEBUG`，由 CMake 设置 |
| GCC / Clang | `-g -fno-omit-frame-pointer`，frame pointer 由 CMake 选项保留 |

GCC/Clang 下会添加 `-rdynamic`，以便非 static 符号能在栈追踪中解析。

## Last-N log ring buffer

每条日志都会被复制进固定、预分配的 ring buffer（`HX_CLOG_RING_CAPACITY` 个 entry）。Crash 时 handler 使用简单 `write()` 调用输出它，不使用 `malloc`，并且 crash 路径不会主动获取普通锁，因此最近的业务日志能进入报告。

## 试运行

使用 `-DHX_CLOG_TEST_TRIGGER_CRASH=ON` 配置，重新构建 `test_crash`（建议 Release 构建——Debug CRT 对无效参数走断言而非报告），并以模式参数运行：`null`（默认，空指针解引用）、`abort`（`abort()`）或 `invalid`（CRT 无效参数，如 `fclose(NULL)`）。每种模式都会按设计终止进程，并在 `./test_crash_logs` 下留下 `crash_*_pid<N>_<序号>.log`，Windows 上还会生成配对的 `.dmp`。触发构建里关闭了 WER 放行以保证运行确定性（不弹系统对话框）。

