# CI/CD

[English](../ci.md) | [中文](ci.md)

本项目使用 GitHub Actions，在每次 push 以及面向 `master` 的 pull request 上构建并测试 `hx_clog`。

## Workflow

Workflow 位于 `.github/workflows/ci.yml`，执行以下检查：

* 在 Ubuntu、Windows、macOS 上分别进行 Debug 和 Release 构建。
* 使用 `HX_CLOG_BUILD_EXAMPLES=ON` 和 `HX_CLOG_BUILD_TESTS=ON` 构建 examples 和 tests。
* 对完整测试套件执行 `ctest --output-on-failure`。
* 原生 **linux-arm**（ubuntu-24.04-arm，aarch64）构建，并在真实 ARM 硬件上运行完整测试套件。这类检查可以捕获错误 syscall number 等架构相关问题。
* **Sanitizer** 构建：一组 ASan+UBSan job 和一组 TSan job，二者都运行完整测试套件，用于捕获内存错误和数据竞争，例如异步引擎 shutdown 竞态。
* **Options matrix**：`HX_CLOG_ENABLE_ASYNC=OFF`、`HX_CLOG_ENABLE_CRASH=OFF`、与 `HX_CLOG_ENABLE_ZLIB=OFF` 的组合，以及 shared-library build，用于验证双重编译门控和稳定的 public ABI stub。
* Linux 安装/打包 smoke test，用于验证导出的 CMake package 和安装后的 public headers。
* Android arm64-v8a 与 iOS arm64 交叉编译 smoke build，且 **开启 crash handler**。Android 使用 `_Unwind_Backtrace`，不依赖 `<execinfo.h>`。

Linux CI 启用 `HX_CLOG_ENABLE_SYSLOG=ON`，确保 syslog sink 路径被编译和链接。Windows CI 覆盖默认 minidump/Event Log 链接依赖。macOS CI 覆盖 Apple 平台构建路径。Ubuntu/macOS 在 zlib 可用时启用它，从而支持将超过 `max_backup_files` 的旧轮转备份压缩为 `.gz`。

## 本地验证

从仓库根目录运行：

```sh
cmake -S . -B build -DHX_CLOG_BUILD_EXAMPLES=ON -DHX_CLOG_BUILD_TESTS=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

Release 检查：

```sh
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Linux syslog 检查：

```sh
cmake -S . -B build-syslog -DCMAKE_BUILD_TYPE=Release -DHX_CLOG_ENABLE_SYSLOG=ON
cmake --build build-syslog --parallel
ctest --test-dir build-syslog --output-on-failure
```

### 启用 zlib 以支持 `.gz` 轮转压缩

`HX_CLOG_ENABLE_ZLIB` 默认 `ON`，但只有当 CMake 的 `find_package(ZLIB)` 找到
zlib 时才真正生效。Linux/macOS 上系统包（`zlib1g-dev` / `brew install zlib`）
会被自动找到；Windows 通常没有系统 zlib，需要手动指向一个。若本机没有，可以从
源码编译一次：

```sh
git clone --depth 1 --branch v1.3.1 https://github.com/madler/zlib.git .zlib/src
cmake -S .zlib/src -B .zlib/build -G "Visual Studio 17 2022" -A x64 \
      -DCMAKE_INSTALL_PREFIX=.zlib/install
cmake --build .zlib/build --config Debug   --target install
cmake --build .zlib/build --config Release --target install
```

**静态 zlib**（运行时无需 DLL，推荐）：

```sh
cmake -S . -B build_zlib -G "Visual Studio 17 2022" -A x64 \
      -DHX_CLOG_ENABLE_ZLIB=ON -DZLIB_USE_STATIC_LIBS=ON \
      -DCMAKE_PREFIX_PATH=.zlib/install
cmake --build build_zlib --config Debug --parallel
ctest --test-dir build_zlib -C Debug --output-on-failure
```

**动态 zlib**（链接导入库；运行时必须能找到 `zlib*.dll`——放在 PATH 上或可执行
文件同目录，否则测试会以 `0xc0000135` 失败）：

```sh
cmake -S . -B build_zlib_dll -G "Visual Studio 17 2022" -A x64 \
      -DHX_CLOG_ENABLE_ZLIB=ON -DCMAKE_PREFIX_PATH=.zlib/install
cmake --build build_zlib_dll --config Debug --parallel
# 先让 DLL 可被找到，再运行测试（PowerShell）：
#   $env:PATH = ".zlib\install\bin;" + $env:PATH
ctest --test-dir build_zlib_dll -C Debug --output-on-failure
```

zlib 生效时，轮转测试会在各测试日志目录下留下 `*.gz` 备份；超过
`max_backup_files` 的旧备份会被压缩而非删除。

## 当前测试覆盖

CTest 套件覆盖：

* Pattern 格式化、级别过滤、basename/full-path 输出。
* 按大小轮转、启动轮转、时间间隔轮转。
* `HX_CLOG_ENABLE_ZLIB` 开启时，对旧轮转备份进行 zlib 压缩。
* 大行处理。
* Crash handler 安装和配置路径。
* 异步投递、阻塞溢出行为、drop-new 溢出计数。
* 命名 logger、线程本地 context、JSON 格式、自定义 formatter、运行时 reconfiguration、sink ID、sink 移除、sink-level 过滤、自定义 allocator 路由、非法文件名校验。
* Per-sink 格式覆盖（pattern 和 JSON）、重复日志折叠（`last message repeated N times`）以及内部错误处理器。
* UTF-8（非 ASCII）日志目录和文件名处理，包括轮转，通过 `test_utf8path` 覆盖 Windows 宽路径层。
* C++11 RAII 封装和轻量 `{}` 格式化辅助函数。

