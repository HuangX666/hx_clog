# hx_clog v1.2.0 — 完备性审计报告

> 审计日期：2026-06-19 | 方法：9 个 Agent 并行扫描全部源码、公开 API、CMake 构建、测试和文档

> **整改状态（2026-06-19 更新）**：本报告以功能完备性/路线图为主，多数条目是增量增强而非缺陷，
> 暂未实现。其中第 4 节“架构风险”的若干并发/健壮性问题已在正确性整改中修复并经 CI 全绿验证 ——
> **M1**（async 计数器无锁撕裂读）、**M5**（轮转清理 OOM 静默）、**M6**（`sigaltstack` 失败仍设
> `SA_ONSTACK`）。详见 [发布完备性审计报告 · 第 0 节 整改状态](2026-06-19-hx-clog-v1.2.0-completeness-audit.md#0-整改状态2026-06-19-更新)。
> 高优先级功能缺口（编译期级别裁剪、TCP/UDP sink、文件/环境配置、Logger 层级继承）与 M2/M3/M4/M7/M8、
> 测试盲区（第 5 节）仍为**开放路线图**，未在本轮处理。

---

## 1. 总体评价

hx_clog 是一个**架构扎实、工程质量好的日志库**。异步引擎、崩溃处理、轮转系统、跨平台抽象都是生产级质量。没有发现死锁、double-free、use-after-free 等严重缺陷。

但它目前是**日志库（logging library）**，还不是**日志框架（logging framework）**。距离 spdlog/glog 级别，差三类高优先级功能和一些测试覆盖。通往"生产级日志框架"的路径清晰且主要是**增量式**的 — 不需要架构重构。

---

## 2. 功能完备性记分卡

| # | 类别 | 状态 | 优先级 | 缺失点 |
|---|------|------|--------|--------|
| 1 | 日志级别过滤 | **部分** | 🔴 高 | 无编译期级别裁剪 (`HX_CLOG_ACTIVE_LEVEL` 不存在) |
| 2 | 输出格式 | **部分** | 🟡 中 | 无 RFC 5424 syslog 结构化数据；无 JSON/logfmt/OTEL 格式 |
| 3 | Sink 类型 | **部分** | 🔴 高 | 无 TCP/UDP 网络 sink；console/file 不可动态管理；硬上限 16 个 sink |
| 4 | 轮转与保留 | **完善** | 🟢 低 | 缺少轮转后上传/归档回调 |
| 5 | 异步/非阻塞 | **完善** | 🟢 低 | — |
| 6 | 限流/去重 | **部分** | 🟡 中 | 仅有重复抑制；无令牌桶/漏桶限流应对不重复的洪水 |
| 7 | 分级 Logger | **部分** | 🔴 高 | 平铺命名空间，无层级继承，无 `find_by_name()`，无枚举遍历 |
| 8 | 编译期日志剔除 | **缺失** | 🔴 高 | 完全不存在 — 所有日志调用在所有构建配置中都编译进二进制 |
| 9 | 作用域上下文/MDC | **部分** | 🟡 中 | 只写不读（无 `context_get()`）；无 RAII scoped guard 自动恢复 |
| 10 | 生命周期管理 | **部分** | 🟡 中 | 无 `atexit()` 自动 flush；shutdown 后 re-init 状态不明确 |
| 11 | 配置管理 | **部分** | 🔴 高 | 无文件配置（INI/JSON/TOML）；env var 有 stub 未完成；无读回 |
| 12 | 性能 | **完善** | 🟢 低 | 稳态零分配；格式检查仅 GCC/Clang（MSVC 缺少 `_Printf_format_string_`） |
| 13 | 测试与 CI | **部分** | 🟡 中 | 无 fuzzing、Valgrind、覆盖率、benchmark、静态分析 |

**汇总：3 完善 / 8 部分 / 2 缺失** — 核心成熟，差距在广度而非深度。

---

## 3. 缺失功能（按影响度排序）

### 高影响

#### 1. 编译期日志级别裁剪 🔴

- **作用：** Release 二进制中 TRACE/DEBUG 调用完全不存在 — 零 CPU、零体积、零字符串表泄漏。
- **复杂度：** 中。需要 `HX_CLOG_ACTIVE_LEVEL` 宏、每个调用点的 `#if` 守卫，以及一套与运行时 `hx_clog_level_t` 枚举对应的预处理器常量。spdlog（`SPDLOG_ACTIVE_LEVEL`）和 glog 展示了标准模式。还需提供 `HX_LOG_*_IF(cond, ...)` 条件宏以避免参数求值。
- **重要性：** 这是嵌入式、游戏引擎、性能敏感场景的入门门槛。没有它，紧凑循环中的每个 TRACE 调用都会进入二进制，格式字符串参数也总是被求值。

#### 2. TCP/UDP 网络 Sink 🔴

- **作用：** 原生远程日志，无需用户自行管理连接生命周期、缓冲、重连和帧协议。
- **复杂度：** 高。需要新 sink 类型，含 socket 管理（非阻塞 connect、退避重连、发送缓冲）、帧协议（换行分隔、RFC 5424 syslog over TCP、或自定义），以及与异步队列的集成，使慢速网络不阻塞生产者。spdlog 的 TCP/UDP sink 和 log4cxx 的 `SocketAppender` 可作为参考。
- **重要性：** Callback sink 逃生舱存在，但把连接管理、缓冲和帧协议全部推给用户。生产日志框架必须内建网络传输。

#### 3. 文件和环境变量配置 🔴

- **作用：** 部署时无需重新编译即可更改配置。运维人员可通过配置文件或环境变量更改日志级别、目录、轮转策略。
- **复杂度：** 中。INI 解析约 200 行 C；JSON 通过嵌入式单头解析器（如 cJSON）增加依赖但支持更丰富的配置。环境变量覆盖（`HX_CLOG_LEVEL`、`HX_CLOG_DIR`、`HX_CLOG_MODE`）已有 stub 在 [hx_clog_core.c](src/hx_clog_core.c) 的 `apply_env_overrides()` 中 — 完成它工作量很低。
- **重要性：** 这是运维团队最大的采用障碍。所有生产日志库（glog 的 `GLOG_*` 变量、log4cxx XML/properties、Boost.Log ini）都提供文件/环境配置。

#### 4. Logger 层级与级别继承 🔴

- **作用：** 将 `"network"` 设为 DEBUG 会传播到 `"network.tcp"`、`"network.http"` 等 — 所有主流日志框架的标准功能。
- **复杂度：** 中。需要树结构（`parent` 指针、`children` 列表在 `hx_clog_logger_t` 上）、`hx_clog_logger_find(name)` 按点分隔前缀查找，以及沿树向上解析级别。spdlog 和 log4cxx 是参考设计。
- **重要性：** 大型模块化代码库（数百个模块）无法实际管理平铺的 logger。没有层级，每个子系统都必须单独配置其日志级别。

#### 5. 配置读回与内省 🔴

- **作用：** `hx_clog_get_config()` 获取当前配置；为所有可设字段提供独立 getter。
- **复杂度：** 低。在 `sink_lock` 下将活跃状态复制到用户提供的 `hx_clog_config_t*`。独立 getter（`hx_clog_get_pattern()`、`hx_clog_get_format_mode()` 等）实现简单。
- **重要性：** 没有读回，封装 hx_clog 的库使用者无法检视自己的日志状态。这是常见的集成痛点。

### 中影响

#### 6. 轮转后上传/归档回调 🟡

- **作用：** 自动将轮转后的日志文件上传至 S3、GCS 或 HDFS；触发外部后处理流水线。
- **复杂度：** 低。在 `hx_clog_config_t` 中添加 `rotate_callback` 字段（签名：`void (*)(const char* archived_path, void* user_data)`），在 `hx_rotate_maybe()` 重命名成功后调用。
- **重要性：** 没有它，每次部署都要重新发明 `inotify`/`ReadDirectoryChangesW` 轮询循环来检测轮转。

#### 7. 限流（令牌桶或漏桶）🟡

- **作用：** 按消息数或字节率限制日志量，防止不同消息的洪水塞满磁盘/网络。
- **复杂度：** 低–中。每个级别或每个 logger 的令牌桶，每次写入时补充。已有的 `dropped_lines` 计数器和错误处理器提供了报告基础设施。
- **重要性：** 重复抑制（1.1.0）是个好开始，但只能捕获完全相同的重复。一个失控循环输出带递增计数器的不同消息会直接穿透。

#### 8. 作用域 MDC 守卫（RAII push/pop）🟡

- **作用：** 作用域退出时自动恢复上下文 — `MDC.push("request_id", id)` 在函数入口，返回时自动弹出。
- **复杂度：** 低。C 语言 `hx_clog_context_push/pop()` 对（由小的每线程栈支持）和 C++ `ScopedContext` RAII 封装。`%x` 占位符已经可以渲染上下文。
- **重要性：** 手动 `context_remove()` 容易出错；忘记清理会导致上下文泄漏到后续日志调用。

#### 9. Atexit 自动 Flush 🟡

- **作用：** 正常程序退出时保证异步队列和文件缓冲区排空，无需显式调用 `hx_clog_shutdown()`。
- **复杂度：** 低。在 init 时用 `atexit()` 注册 `hx_clog_flush()`；保持幂等。spdlog 默认这样做。
- **重要性：** 异步日志最常见的支持问题是"崩溃/退出前丢失了最后 5 秒的日志"。崩溃处理器覆盖了崩溃路径；atexit 覆盖正常退出。

#### 10. Logger 查找与枚举 🟡

- **作用：** `hx_clog_logger_find("network.tcp")` 获取已有 logger 而不创建重复；`hx_clog_logger_count()`/迭代用于仪表盘/工具。
- **复杂度：** 低。维护命名 logger 的链表或动态数组；添加哈希表或排序数组实现 O(log n) 查找。
- **重要性：** 没有查找，丢失 logger 指针意味着以后无法重新配置它。

---

## 4. 架构风险（按严重度排序）

### 中等 — 8 项

**M1. 异步 drop 计数器数据竞争（32 位平台撕裂读）**
- **位置：** [hx_clog_async.c:413-418](src/hx_clog_async.c)，[hx_clog_core.c:1436-1438](src/hx_clog_core.c)
- **风险：** `hx_async_dropped()` 和 `hx_async_high_watermark()` 在无锁情况下读取 `unsigned long long` 字段，而写操作是有锁的。在 32 位平台上这是撕裂读；即使在 64 位上也是 C11 数据竞争（UB）。仅影响统计，运行影响低。
- **修复复杂度：** 低。在 `g_async_lock` 下读取，或声明为 `_Atomic unsigned long long`。

**M2. 未知编译器上原子退化为非原子普通存储**
- **位置：** [hx_clog_core.c:227-244](src/hx_clog_core.c)
- **风险：** 非 MSVC/非 `__GNUC__` 编译器的 `#else` 分支执行 `*p = v` / `return *p` — 非原子。不定义 `__GNUC__` 的编译器（Oracle Developer Studio、TinyCC、旧版 ICC）在全局级别字段上产生数据竞争。`hx_clog_set_level()` 与热路径级别检查竞争可能导致日志丢失。
- **修复复杂度：** 低。使用 C11 `_Atomic`/`stdatomic.h` 作为回退，或对不支持的编译器 `#error`。

**M3. 非 GCC/non-MSVC 编译器 TLS 静默退化为全局变量**
- **位置：** [hx_clog_core.c:22-27](src/hx_clog_core.c)
- **风险：** `HX_THREAD_LOCAL` 在不支持的编译器上映射为空。TLS 上下文和格式缓存变为全局 — 多线程互相覆盖 MDC，且每次日志调用都强制获取 `sink_lock`（格式缓存失效）。
- **修复复杂度：** 低。使用 C11 `_Thread_local`，或 `#error`。

**M4. Ring Buffer 初始化 OOM 静默 — 所有 ring push 变为空操作**
- **位置：** [hx_clog_core.c:481-494](src/hx_clog_core.c)
- **风险：** 如果 `hx_clog__malloc` 对 512 KB ring buffer 失败，`g_ring` 保持 NULL。所有后续 `hx_ring_push()` 调用静默空操作。崩溃处理器在崩溃报告中找不到"最后 N 条日志"，且无任何指示说明 ring buffer 从未分配。
- **修复复杂度：** 低。通过错误处理器报告；可选择从 init 返回错误。

**M5. 轮转清理 OOM 静默跳过 — 备份清理被放弃**
- **位置：** [hx_clog_rotate.c:358-367](src/hx_clog_rotate.c)
- **风险：** `hx_rotate_cleanup()` malloc 4 个数组（~140 KB）。如果任一失败，静默返回而不调用 `hx_core_report_error()`。轮转本身成功（文件已重命名、重新打开），数据安全，但旧备份无限累积，操作员毫无感知。
- **修复复杂度：** 低。在返回前调用 `hx_core_report_error()`。

**M6. `sigaltstack` 失败未处理 — 仍带 `SA_ONSTACK` 安装崩溃处理器**
- **位置：** [hx_clog_crash.c:663-669](src/hx_clog_crash.c)
- **风险：** 在不支持 `sigaltstack` 的嵌入式/POSIX-lite 平台上，该函数静默失败。`sigaction()` 仍被调用并带 `SA_ONSTACK` 标志；内核可能忽略该标志或调用失败。每个信号的 `sigaction()` 返回值未检查。
- **修复复杂度：** 低。检查 `sigaction()` 返回值；失败时无 `SA_ONSTACK` 重试。

**M7. 异步批量写期间移除 Callback Sink — 悬垂指针**
- **位置：** [hx_clog_core.c:885-908](src/hx_clog_core.c)（sink 分发）；[hx_clog_async.c:301](src/hx_clog_async.c)（worker 释放队列锁后写入）
- **风险：** `hx_clog_remove_sink()` 在 `sink_lock` 下刷新异步队列并压缩 sink 数组，但异步 worker 在 `g_async_lock` 下复制行数据，释放锁后调用 `hx_core_emit_to_sinks()`。如果在队列锁释放和 emit 之间 callback sink 被移除，worker 持有悬垂指针。
- **修复复杂度：** 中。添加引用计数或代际号到 sink，或要求 sink 只能由创建它的线程移除（作为契约文档化）。

**M8. pkg-config `Libs.private` 在 ARM32/MIPS 上缺少 `-latomic`**
- **位置：** [CMakeLists.txt:224-240](CMakeLists.txt)；`cmake/hx_clog.pc.in`
- **风险：** 在需要 `-latomic` 实现 64 位原子操作的平台（旧版 ARM32、MIPS）上，静态链接失败，出现未定义符号。
- **修复复杂度：** 低。使用 CMake 的 `CheckCXXSourceCompiles` 检测 `-latomic` 需求并追加到 `HX_CLOG_PC_LIBS_PRIVATE`。

### 低 — 16 项

| 编号 | 位置 | 问题 |
|------|------|------|
| L1 | `core.c` | 全局 `sink_lock` 串行化所有 sink IO — 吞吐量天花板，非 bug |
| L2 | `rotate.c` | `gzip_file()` 在轮转清理期间持有 `fs->lock` — 对同一 sink 的其他写入者产生延迟尖峰 |
| L3 | `core.c:2019-2023` | `vsnprintf` 编码失败静默返回（不调用错误处理器） |
| L4 | `rotate.c:510-665 vs 668-731` | `rotate_force()` 复制 `rotate_maybe()` 约 70% 代码 — 维护风险 |
| L5 | `crash.c` | Windows 崩溃路径上 ring buffer dump 在无锁情况下读取 `g_ring_head`/`g_ring_count` — 崩溃报告中可能出现撕裂条目 |
| L6 | `core.c` | 非 Linux/非 Apple POSIX 上将 `pthread_self()` 强转为 `unsigned long` — 在 BSD 上损坏（`pthread_t` 是结构体） |
| L7 | `internal.h` | `HX_CLOG_PATH_MAX`（1024）静默截断深层嵌套路径而非检测溢出 |
| L8 | `async.c:271-282` | Worker scratch buffer OOM 静默丢弃行而不报错 |
| L9 | `core.c` | `format_with_retry` 用 `strlen()` 而非 `vsnprintf` 返回值 — 实践中等价，但脆弱 |
| L10 | `file.c:92-105` | 同一写失败转换的错误处理器可能因 TOCTOU 被调用两次 |
| L11 | `rotate.c` | `rotate_force` 用 `time(NULL)` 而非 `ts.sec` 做退避参考 — 退避窗口短于预期 |
| L12 | `hx_clog.h` | 公开枚举值隐式定义 — 在 `OFF` 前添加新级别会移位并破坏 ABI |
| L13 | `hx_clog.h` | `hx_clog_config_t` 无 `size`/`version` 字段 — 仅靠末尾追加约定维持前向兼容 |
| L14 | `tests/CMakeLists.txt` | 测试 `add_test()` 调用无 `WORKING_DIRECTORY` — 在特定 IDE 测试运行器下可能失败 |
| L15 | `crash.c` | ARM32/RISC-V/MIPS/POWER/s390x 上寄存器 dump 不可用（栈回溯可用） |
| L16 | `rotate.c` | 手写插入排序代替 `qsort()` — 512 条目 O(n²)，可接受 |

---

## 5. 测试覆盖盲区（按未检出 bug 的风险排序）

### 严重风险

**T1. `HX_CLOG_OVERFLOW_DROP_OLD` — 零覆盖**
- [hx_clog_async.c:154-158](src/hx_clog_async.c) 通过推进 `head` 和递减 `count` 实现 DROP_OLD。只有 BLOCK 和 DROP_NEW 被测试。head 推进逻辑中的 bug 会静默破坏队列。**零测试。**

**T2. 多线程并发写入 — 零覆盖**
- 所有测试都是单线程、单生产者。异步队列在并发生产者下的表现 — 设计的全部目的 — 从未被验证。mutex/condvar 原语仅在平凡的单生产者单 worker 模式下使用。**零测试。**

**T3. 自定义分配器 OOM 处理 — 零覆盖**
- `core.c`、`async.c` 和 `rotate.c` 中 6 条不同的 OOM 路径从未被触发。`test_features.c` 安装了一个始终成功的分配器。任何 OOM 路径中的空指针解引用都不会被检测到。**零测试。**

**T4. 异步日志活跃期间移除 Callback Sink — 零覆盖**
- 没有测试在异步 worker 排空期间移除 sink。worker 批量写入路径中的 use-after-free 是可能的。**零测试。**

### 高风险

**T5. 日轮转（`rotate_daily=1`）— 零覆盖**
- 所有轮转测试中 `rotate_daily` 都是 `0`。[hx_clog_rotate.c:542-548](src/hx_clog_rotate.c) 中 `hx_rotate_maybe()` 的 `cur_yday`/`cur_year` 日边界检查从未被覆盖。修复日边界问题的提交没有回归测试。

**T6. `rotate_align` 时钟对齐 — 零覆盖**
- 没有测试设置非零 `rotate_align`。[hx_clog_rotate.c:555-566](src/hx_clog_rotate.c) 的桶边界计算从测试角度看是死代码。

**T7. `hx_clog_after_fork_child()` — 零覆盖**
- 重新初始化每个 mutex、condvar、ring buffer，并重启异步 worker。子进程中的死锁会是静默且灾难性的。天然难以测试（需要 `fork()`），但缺失意味着锁重初始化序列从未被验证。**零测试。**

**T8. 文件 Sink 内容验证 — 零覆盖**
- 所有内容断言使用内存 callback sink。没有测试读回文件字节以验证正确的行内容。如果 `fwrite` 成功但写入垃圾数据，没有测试能捕获。**零内容测试。**

**T9. `hx_clog_reopen()` — 从未调用**
- 没有测试调用 reopen。关闭并重新打开的逻辑从未被验证。**零测试。**

**T10. 控制台 Sink ANSI 颜色 / stderr 路由 — 零覆盖**
- 所有测试设置 `enable_console = 0`。控制台 sink — 级别路由、ANSI 颜色、`isatty` 门控 — 从未被测试覆盖。

### 中风险

| 编号 | 问题 |
|------|------|
| T11 | 异步模式下按 sink 格式覆盖 — 仅在同步模式下测试 |
| T12 | `max_backup_days` 按时间清理 — 从测试角度看是死代码 |
| T13 | `hx_clog_flush_sink()` 按 sink flush — 从未调用，只用全局 `hx_clog_flush()` |
| T14 | 亚秒级轮转间隔 — 提交 `8c2a858` 修复了 bug 但未添加测试 |
| T15 | `HX_LOG_NAMED_*` 和 `HX_LOGGER_*` 宏 — 仅 WARN/ERROR 级别被测试 |
| T16 | 系统 sink（syslog/eventlog/logcat/os_log）— 全平台零覆盖 |
| T17 | 崩溃 ring buffer 内容 — `test_crash.c` 触发崩溃但不检查 `crash_*.log` 内容 |
| T18 | `hx_clog_context_remove()` 单键移除 — 未测试 |
| T19 | `vsnprintf` 返回负值 — 错误路径未覆盖 |
| T20 | 环境变量覆盖（`apply_env_overrides`）— 零覆盖 |

---

## 6. 文档缺口

### 未文档化的 API（约 40+ 符号存在于代码中，README 第 5 章未收录）

README 第 5 章（"C API design"）记录了约 15 个公开符号，而 `include/hx_clog.h` 中实际有约 60 个。`docs/api.md` 是权威参考且更完整。差距主要在 README 第 5 章与实际 API 表面之间：

**README 5.2 缺失的 Config 结构体字段（27 个字段中缺 12 个）：**
`enable_syslog`、`enable_event_log`、`enable_android_log`、`enable_apple_log`、`rotate_interval_seconds`、`rotate_on_startup`、`overflow_policy`、`format_mode`、`formatter`、`formatter_user_data`、`system_logger_name`、`max_backup_days`、`rotate_align`、`max_compressed_files`、`date_subdir`

**README 5.3 缺失的函数/API：**
- 生命周期：`hx_clog_is_initialized()`、`hx_clog_reconfigure()`、`hx_clog_after_fork_child()`
- 格式：`hx_clog_set_pattern()`、`hx_clog_set_format_mode()`、`hx_clog_set_formatter()`、`hx_clog_set_sink_pattern()`、`hx_clog_set_sink_format_mode()`
- 命名 Logger：`hx_clog_logger_create/destroy/set_level/get_level/name/write/writev()`、`hx_clog_write_named()`、`hx_clog_writev_named()`、`hx_clog_default_logger()`
- 上下文：`hx_clog_context_put/remove/clear()`
- 统计：`hx_clog_get_stats()`、`hx_clog_stats_t`
- Sink：`hx_clog_add_callback_sink_ex()`、`hx_clog_remove_sink()`、`hx_clog_set_sink_level()`、`hx_clog_flush_sink()`、`hx_clog_get_sink_count()`、全部四个 `hx_clog_add_*_log_sink()` 系统 sink 工厂函数
- 其他：`hx_clog_set_allocator()`、`hx_clog_set_duplicate_suppression()`、`hx_clog_set_error_handler()`、`hx_clog_set_crash_callback()`、`hx_clog_level_name()`、`hx_clog_level_from_name()`
- 宏：`HX_LOG_NAMED_*`、`HX_LOGGER_*`
- 类型：`hx_clog_record_t`、`hx_clog_formatter_t`、`hx_clog_crash_callback_t`、`hx_clog_error_handler_t`

### 文档描述但未实现（3 项）

1. **`max_total_size`** — README 第 11.4 节列出了"日志目录总大小限制"1 GB。`hx_clog_config_t` 中不存在此字段，`hx_clog_rotate.c` 中无对应代码。
2. **INI 配置文件支持** — README 第 19.1 节描述了 `[hx_clog]` INI 段和"可选扩展"。不存在 INI 解析器或 `hx_clog_load_config_from_file()` 函数。
3. **后台压缩线程** — README 第 11.2 节说"压缩应在后台线程中运行"。实际实现在 `hx_rotate_cleanup()` 中持有 `fs->lock` 进行内联压缩。

### 错误值

- **`max_backup_files` 默认值：** README 第 24 章写 `10`；代码实际默认是 `-1`（无限/永不删除）。结构体注释和 1.1.0 更新日志正确地写着 `-1`。
- **README 5.2 Config 结构体：** 显示 15 个字段；实际有 27 个。缺少所有 1.1.0/1.2.0 新增项。
- **README 第 24 章默认配置：** 设置 15 个字段；`hx_clog_config_default()` 设置全部 27 个。

---

## 7. 结论：是否生产就绪？

### 场景评估

| 场景 | 是否就绪 |
|------|----------|
| 单进程、单机、编译期配置的部署 | ✅ **是** |
| 多服务、动态配置、网络化部署 | ❌ **尚未** |

### MVP（最小可用 — ~4 周）

| 项目 | 工作量 | 理由 |
|------|--------|------|
| 编译期级别裁剪 | 中 | 消除性能/嵌入式用户的最大反对理由；spdlog/glog 基准线 |
| 环境变量覆盖 | 低 | `apply_env_overrides()` stub 已存在；无需文件解析即可完成配置故事 |
| DROP_OLD 测试 + 多线程并发测试 | 中 | 关闭两个最高风险的测试盲区；验证异步引擎的核心承诺 |
| Callback sink 移除安全性（文档化或修复） | 低 | 文档说明 sink 必须由创建线程移除，或添加代际计数器 |

### 完整生产就绪（追加 ~6-8 周）

| 项目 | 工作量 |
|------|--------|
| TCP/UDP 网络 sink | 高 |
| 文件配置（INI 或 JSON） | 中 |
| Logger 层级 + find-by-name | 中 |
| 轮转后 hook | 低 |
| 限流（令牌桶） | 低–中 |
| 配置读回 / `hx_clog_get_config()` | 低 |
| 作用域 MDC 守卫（C + C++） | 低 |
| Atexit 自动 flush | 低 |
| Fuzzing 夹具（格式引擎 + 配置解析器） | 中 |
| CI 中 Valgrind + 覆盖率 | 低 |
| README 第 5 章更新以匹配 `docs/api.md` | 低 |

### 总体判断

hx_clog v1.2.0 是一个**内部架构干净的优秀 C 日志库**，其质量远超其文档和测试覆盖所体现的水平。在同版本号下，它比大多数 C 日志项目更接近生产就绪。通往"生产级日志框架"的路径清晰、范围明确，且主要是**增量式**的 — 不需要架构重构。三个高优先级功能缺口（编译期裁剪、网络 sink、文件/环境配置）加上严重测试盲区（DROP_OLD、多线程并发、OOM 恢复）是最低门槛。解决这些后，hx_clog 将进入 C 生态中与 spdlog 和 glog 同级的梯队。
