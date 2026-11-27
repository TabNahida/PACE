<!-- source-hash: 2f3b3a4bd584 -->
# 持续集成与架构覆盖

[English](ci.md) · [参与贡献](README.zh.md)

PACE 的 CI 把原生执行、模拟执行和针对外部 SDK 的编译区分开。仅编译的任务通过，并不能证明网络代码在该设备上
运行过。下表描述的是配置的检查项；具体哪个版本、哪些任务通过，以[工作流结果](https://github.com/TabNahida/PACE/actions)
为准。CI 只检查正确性：它丢弃基准测试的计时数据，不发布性能排名。公开的性能数据来自本地 Linux。

## 工作流

工作流位于 `.github/workflows/`。除测量类工作流外，其余工作流都会在推送到 `main` 和提交 pull request 时运行，
也可以手动启动。

| 工作流 | 文件 | 检查内容 |
|---|---|---|
| 构建与功能验证 | `ci.yml` | Linux、macOS、Windows 和 FreeBSD 上的原生构建与测试；sanitizer；逐个模块单独构建 |
| 架构可移植性 | `portability.yml` | 四种 Linux 架构上的 QEMU 执行、Android 与 iOS 编译、MinGW |
| QUIC 后端与新编译器 | `quic-backends.yml` | 两种 QUIC 后端配合 GCC 15、GCC 16 和 Clang 21；命名模块；后端之间的互通 |
| C++ 命名模块 | `modules.yml` | 用 Clang 18 在三种配置下 `import pace` |
| 代码风格 | `style.yml` | clang-format、生成的模块接口 |
| 静态分析 | `tidy.yml` | 对改动涉及的行运行 clang-tidy |
| 文档 | `docs.yml` | 检查译文、结构、链接与 README 配图；用 `mkdocs build --strict` 构建站点，启用 GitHub Pages 后从 `main` 发布 |
| 模糊测试 | `fuzz.yml` | 对 HTTP/1 和 WebSocket 解析器运行 libFuzzer；在 macOS 和 Windows 上回放种子语料 |
| 互通测试 | `interop.yml` | curl、`openssl s_client` 和 h2spec 对接 PACE 服务端 |
| 基准基线 | `baseline.yml` | 手动：为某个版本运行全部基准套件，原始结果作为产物上传 |
| 网络与运行时性能 | `network-performance.yml`、`runtime-performance.yml` | 手动或在测量分支上：在托管 runner 上做配对比较 |

## 原生执行

`ci.yml` 主矩阵的每一项都会构建运行时、TCP/UDP、该平台支持的全部协议、测试和教程，然后运行 `xmake test -v`
以及独立的 Python WebSocket 对端。执行前会检查可执行文件的 ELF、PE 或 Mach-O 头是否与所要求的 CPU 和字长一致。
尤其是 ARM Windows 任务，不可能因为意外构建出 x64 可执行文件、再经 Windows 模拟运行而蒙混通过。

| 操作系统 / runner | 目标 | 编译器 / 标准 | 协议 | I/O 后端 |
|---|---|---|---|---|
| Ubuntu 24.04 | x86_64 | GCC，C++20 和 C++23 | 全部 | epoll |
| Ubuntu 24.04 | x86_64 | GCC，C++20，`--uring=y` | 全部 | io_uring |
| Ubuntu 24.04 | x86_64 | Clang，C++20 | 全部 | epoll |
| Ubuntu 24.04 ARM | aarch64 | GCC，C++20 | 全部 | epoll |
| Ubuntu 24.04 | i386，x86_64 Linux 上的 32 位进程 | GCC multilib，C++20 | 全部 | epoll |
| macOS 15 Intel | x86_64 | Apple Clang，C++20 | 全部 | kqueue |
| macOS 15 | arm64 | Apple Clang，C++20 | 全部 | kqueue |
| Windows 2025 | x64 | MSVC，C++20 | 除 QUIC/HTTP3 外全部 | IOCP |
| Windows 2025 | x86 | MSVC，C++20 | 除 QUIC/HTTP3 外全部 | WSAPoll |
| Windows 11 ARM | arm64 | MSVC，C++20 | 除 QUIC/HTTP3 外全部 | IOCP |
| FreeBSD 14.3 虚拟机 | x86_64 | Clang，C++20 | 全部 | kqueue |

“全部”指 HTTP1、WebSocket、TLS、DNS、QUIC、HTTP2 和 HTTP3。Windows 不支持 QUIC 和 HTTP3；Windows 任务会检查
配置这两项时是否给出明确的错误。Windows ARM64 使用一项范围受限的 [OpenSSL 构建修正](windows-arm64-openssl.zh.md)，
该任务还会运行独立的 OpenSSL 诊断程序，并把其符号文件保存为产物。

`ci.yml` 另有三个 Linux 任务，覆盖矩阵没有覆盖的内容：

- **Sanitizer**：以 debug 构建启用全部协议，开启 ASan、UBSan 和泄漏检测；任何报告都视为失败。
- **逐个模块单独构建**：先只构建核心，再分别单独启用 TLS、DNS、QUIC、HTTP1、HTTP2、HTTP3 和 WebSocket，并加上
  `--header_check=y`，把每个公开头文件放在独立的翻译单元中编译。悄悄依赖其他模块的模块、或者缺少 include 的
  头文件，都会在这里失败。
- **FreeBSD**：上表中的 FreeBSD 一行在虚拟机中运行，执行同样的测试、Python 对端和基准检查。

x86_64 GCC、Intel macOS、x64 Windows 和 FreeBSD 几项还会检查基准测试的校验和、结果格式以及插桩运行时的计数器
（另做一次 `--profiling=y` 构建）。这些都是小规模的正确性负载：计时数据会被丢弃。

HTTP/2 背压场景各有一个 30 秒的看门狗。刻意调小的 socket 缓冲区可能与 FreeBSD 的 TCP 分段和延迟 ACK 相互作用，
如果整个不断增长的测试套件共用一个期限，后面的场景仍在推进时期限就可能耗尽。按场景设置上限后，负载、4 KiB
缓冲区设置和正确性断言都保持不变。FreeBSD 为失败情况开启可选的 HTTP/2 跟踪；这些是正确性诊断，不是性能测量。

## 模拟执行的 Linux

在 `portability.yml` 中，Ubuntu 交叉编译器以 C++20 构建 release 可执行文件，然后由显式指定的 QEMU 用户态模拟器
运行全部已启用的测试套件和教程，包括回环 TCP/UDP、HTTP 和 WebSocket。不依赖隐式的 `binfmt_misc` 设置。
运行前会验证每个可执行文件的 ELF 机器类型、字长和字节序。

| 目标 | 编译器前缀 | 模拟器 | 字长 / 字节序 |
|---|---|---|---|
| ARMv7 硬浮点 | `arm-linux-gnueabihf-` | `qemu-arm` | 32 位 / 小端 |
| RISC-V64 | `riscv64-linux-gnu-` | `qemu-riscv64` | 64 位 / 小端 |
| PowerPC64 LE | `powerpc64le-linux-gnu-` | `qemu-ppc64le` | 64 位 / 小端 |
| IBM Z / s390x | `s390x-linux-gnu-` | `qemu-s390x` | 64 位 / 大端 |

这些任务覆盖运行时、TCP/UDP、HTTP1 和 WebSocket，不编译 TLS、DNS、QUIC、HTTP2 或 HTTP3。QEMU 把 CPU 指令和
Linux 系统调用转换到宿主内核上执行；这能检验目标 ABI 和字节序，但不能验证真实目标的内核、硬件内存模型或性能。

安装 `g++-arm-linux-gnueabihf` 和 `qemu-user` 之后，本地的等价命令是：

```sh
export XMAKE_CONFIGDIR=/tmp/pace-armv7-ci-config
xmake f -c -p linux -a armv7 --toolchain=cross \
  --cross=arm-linux-gnueabihf- --bin=/usr/bin -m release \
  -o /tmp/pace-armv7-ci-build --cxxstandard=cxx20 \
  --http1=y --websocket=y --tls=n --dns=n --quic=n --http2=n --http3=n \
  --benchmarks=n --components=n --scaling=n --profiling=n --sanitizers=n \
  --examples=y --tests=y -y
xmake -y
python3 scripts/ci_portability.py --build-dir /tmp/pace-armv7-ci-build \
  --target armv7 --mode qemu-user --emulator qemu-arm \
  --sysroot /usr/arm-linux-gnueabihf --output /tmp/pace-armv7-ci.json
```

`portability.yml` 还在 Windows 2025 上用 MinGW GCC（MSYS2 UCRT64）构建，并原生运行核心、HTTP1 和 WebSocket
测试套件、教程以及 Python WebSocket 对端。它是一项不带可选第三方协议依赖的额外编译器检查，而不是第二套完整的
依赖矩阵。

## 仅编译的移动 SDK

| SDK / 构建主机 | 目标 | 范围 |
|---|---|---|
| Android NDK 27.3.13750724，API 30 / Ubuntu 24.04 | armeabi-v7a、arm64-v8a、x86_64 | 仅静态库 |
| iOS SDK，最低 iOS 15 / macOS 15 | arm64 真机和 arm64 模拟器 | 仅静态库 |

两项 SDK 检查都在没有可选依赖的情况下编译运行时、TCP/UDP、HTTP1 和 WebSocket，并检查归档成员是否为预期的
ELF/Mach-O 架构。它们不链接或签名应用，不启动模拟器，也不运行网络测试。因此设备生命周期、entitlement、沙箱限制
和 socket 行为都未经验证；这些目标不被声明为受支持的运行平台。WebAssembly 和 32 位 Apple 目标被略去，
因为 socket 后端不支持它们。

## QUIC 后端与新编译器

`quic-backends.yml` 分别使用 GCC 15、GCC 16.2 和 Clang 21，以两种 QUIC 后端（`--quic_backend=openssl` 和 `ngtcp2`）
构建全部协议，并运行全部测试和教程。使用 GCC 16 和 Clang 21 时，它还会构建包含全部协议的命名模块，并运行
`pace_module_tests`，在不同翻译单元中混用头文件和模块的使用方；GCC 15 跳过这一步，因为它在导入内联协程时会出现
编译器内部错误。最后一个任务让用不同后端构建的客户端和服务端双向交换数据。

## 命名模块

`modules.yml` 使用 Clang 18 及其模块扫描器，在三种配置下构建 `import pace`：只有核心的 C++20、默认模块
（HTTP1 和 WebSocket）的 C++20，以及启用全部协议的 C++23。它运行已启用的测试，包括跨翻译单元的 ABI 测试。

## 代码风格、静态分析与模糊测试

- `style.yml` 安装固定版本的 clang-format 22.1.8，运行 `python scripts/format.py --check`，再运行
  `python scripts/generate_modules.py --check`，确认命名模块接口与 `modules/exports.json` 一致。
- `tidy.yml` 在 Linux 上启用全部协议，导出编译命令，并用固定版本的 clang-tidy 22.1.8 检查改动涉及的
  `include/` 和 `src/` 中的行；任何警告都会让任务失败。`.clang-tidy` 列出了启用的检查以及关闭某些检查的理由。
- `fuzz.yml` 用 libFuzzer、ASan 和 UBSan（Clang 21，`--fuzz=libfuzzer`）构建 `http1_server`、`http1_client` 和
  `websocket_server` 三个目标，推送和 pull request 时每个目标模糊测试 60 秒，每晚 30 分钟。工作语料在运行之间
  缓存，导致崩溃的输入作为产物上传。`tests/fuzz/corpus` 中的种子语料在 macOS 15 和 Windows 2025 上作为普通测试
  回放（`--fuzz=replay`）。

## 与外部对端的互通

`interop.yml` 构建启用全部协议的 `pace_interop_server`，并对它运行 `tests/interop/run_interop.py`：基于 HTTP/1.1、
HTTPS、h2c 和 h2 的 curl，`openssl s_client`，以及 h2spec 2.6.0。在已知失败项记录到
`tests/interop/h2spec-known-failures.txt` 之前，h2spec 的结果仅供参考。`tests/interop/peers.json` 固定了各对端的
版本，并列出后续阶段要加入的对端。

## 性能工作流

`baseline.yml` 需要手动启动。它针对某个 tag、分支或提交，在 Linux x86_64、Linux arm64 和 macOS arm64 上构建全部
基准套件，用 `scripts/run_baseline.py` 测量并上传原始结果。托管 runner 是共享的，噪声很大：这些产物只能与同一
工作流的其他运行结果比较，公开的数字应在安静的本地 Linux 机器上测量。`network-performance.yml` 和
`runtime-performance.yml` 在手动启动或推送到测量分支时，对每种 I/O 后端（包括 FreeBSD）做配对比较；它们同样只
生成产物和任务摘要。

## 运行时间与维护成本

任务会取消被取代的旧运行，使用只读的仓库权限，并有明确的超时上限。协议依赖缓存按操作系统、目标架构和编译器
分开。模拟执行和移动端任务不构建第三方协议；基准格式和 profiling 检查只在每个主要桌面系统和 FreeBSD 上各运行
一次。不允许任何任务悄悄失败。可移植性产物记录目标、执行方式、检查过的二进制元数据和通过的功能检查；仅编译和
仅元数据的报告会明确说明没有运行测试。

添加这个矩阵时，runner 标签和 SDK 的选择对照了以下官方资料：

- [xmake-repo 工作流](https://github.com/xmake-io/xmake-repo/tree/dev/.github/workflows)
  为原生 ARM、分开的 Apple 架构、交叉编译器和移动 SDK 任务提供了参考。PACE 使用一套更小的依赖矩阵，
  针对自己的 epoll、kqueue 和 WSAPoll 后端裁剪。
- [GitHub 托管 runner 参考](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)
  列出了明确的 Ubuntu 24.04 ARM、Windows 11 ARM 和 macOS 15 Intel 标签。
- [Ubuntu 24.04 镜像内容](https://github.com/actions/runner-images/blob/main/images/ubuntu/Ubuntu2404-Readme.md)
  列出了 Android SDK；如果镜像中没有固定版本的 NDK，工作流会自行安装。
- [QEMU 用户态文档](https://www.qemu.org/docs/master/user/main.html)
  说明了系统调用转换以及模拟执行的局限。

## 显式 I/O 后端覆盖

一个 Linux 原生任务以 `--uring=y` 构建，并在 `PACE_IO_BACKEND=io_uring` 下运行完整测试套件。Windows x64 和 ARM64
显式选择 IOCP；x86 选择 WSAPoll，因为独立的 SDK 探测在 WoW64 下会拒绝通知注册。后端测试验证所选实现、队列压力、
取消和 socket 复用。网关测试覆盖路径限制、缓存策略、连接池、负载均衡和期限。远程 runner 只提供正确性结果。
参见 [I/O 后端](../reference/runtime/io-backends.zh.md)。
