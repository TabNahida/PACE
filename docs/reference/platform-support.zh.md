<!-- source-hash: 9ca7ff09bbe1 -->
# 平台支持

[English](platform-support.md) · [参考](README.zh.md)

## 概述

PACE 运行在 Linux、macOS、FreeBSD 和 Windows 上。除需要 POSIX 平台的 QUIC 和 HTTP/3 外，所有模块在这四个平台上都
可用。本页列出支持的平台与编译器、CI 在各平台上检查什么、各平台使用的 I/O 后端，以及公共 API 中与平台相关的类型
和行为。

| 平台 | CI 中运行的架构 | 模块 | I/O 后端 |
|:--|:--|:--|:--|
| Linux | x86_64、aarch64、i386（32 位进程） | 全部 | epoll（默认）、io_uring（`--uring=y`） |
| macOS | x86_64、arm64 | 全部 | kqueue |
| FreeBSD | x86_64 | 全部 | kqueue |
| Windows（MSVC） | x64、x86、arm64 | 除 QUIC 和 HTTP/3 外全部 | IOCP 套接字通知、WSAPoll |
| Windows（MinGW） | x86_64 | 已测试核心、HTTP/1 和 WebSocket；不支持 QUIC 和 HTTP/3 | IOCP 套接字通知、WSAPoll |

## 编译器

PACE 使用 C++20（可用 `--cxxstandard` 选择 C++23 及更新标准）。支持的最低编译器版本：

| 编译器 | 最低版本 |
|:--|:--|
| GCC | 13 |
| Clang | 16 |
| Apple Clang | 16（Xcode 16） |
| MSVC | 19.38（Visual Studio 2022 17.8） |

每个 PACE 头文件都会包含 `<pace/runtime/requirements.hpp>`：编译器版本过低或语言模式低于 C++20 时，它用 `#error`
指出具体要求。[最低编译器](../../.github/workflows/compilers.yml)工作流正是用这些版本构建全部支持的模块并运行测试。
GCC 12 能编译 PACE，但异常穿过协程挂起点时会让程序终止；Apple Clang 15 已经没有 CI runner 可以测试；所以两者都不在
支持范围内。

其他工作流使用各运行器自带的编译器构建和测试：

| CI 任务 | 编译器 |
|:--|:--|
| Linux x86_64、aarch64、i386；C++20 与 C++23 | Ubuntu 24.04 的 GCC（GCC 13） |
| Linux x86_64 | Ubuntu 24.04 的 Clang（Clang 18） |
| QUIC 后端，两个后端加全部模块 | GCC 15、GCC 16.2、Clang 21 |
| C++ 命名模块 | Clang 18（模块任务），GCC 16 与 Clang 21（QUIC 后端任务） |
| macOS 15，x86_64 与 arm64 | 运行器默认 Xcode 的 Apple Clang |
| FreeBSD 14.3 | 系统自带的 Clang |
| Windows Server 2025 与 Windows 11 ARM | 运行器上 Visual Studio 2022 的 MSVC |
| Windows MinGW | MSYS2 UCRT64 的 GCC |
| Linux armv7、riscv64、ppc64le、s390x（模拟） | Ubuntu 24.04 交叉编译 GCC |
| Android、iOS（仅编译） | NDK 27 Clang、Xcode Clang |

C++ 命名模块（`--modules=y`）还需要 Clang 或 GCC 16；见[构建选项](build-options.zh.md)。

## CI 检查的内容

[CI 覆盖指南](../contributing/ci.zh.md)列出了每个任务；简而言之：

- **原生执行，全部模块**：Linux x86_64（GCC 与 Clang，C++20 与 C++23，另有一个 io_uring 任务）、Linux aarch64、
  Linux i386、macOS x86_64 与 arm64、FreeBSD x86_64。每个任务都构建并运行所有测试和教程，并与独立的 Python
  WebSocket 对端互通。
- **原生执行，除 QUIC 和 HTTP/3 外的全部模块**：使用 MSVC 的 Windows x64（IOCP）、x86（WSAPoll）和 arm64（IOCP）。
  这些任务还检查配置 QUIC 时是否以预期的错误失败。
- **原生执行，核心、HTTP/1 与 WebSocket**：使用 MinGW 的 Windows x86_64。
- **模拟执行，核心、HTTP/1 与 WebSocket**：在 QEMU 用户模式下运行的 Linux armv7、riscv64、ppc64le 和 s390x。它们
  运行在宿主内核上，不能说明原生硬件上的表现或性能。
- **仅编译，核心、HTTP/1 与 WebSocket**：Android（armeabi-v7a、arm64-v8a、x86_64；API 30）和 iOS arm64（真机与
  模拟器）。这些平台上的网络功能、应用集成和设备限制尚未验证。
- **仅 Linux**：启用全部模块的 AddressSanitizer、UndefinedBehaviorSanitizer 与内存泄漏检查；启用
  [头文件检查](build-options.zh.md#头文件自包含检查)单独构建并测试每个模块；两个 QUIC 后端之间相互交换数据。

这里的“支持”指“按上述方式构建和测试”，并不意味着性能相同。公开的性能数据来自本地 Linux 测量；见
[性能](../performance/README.zh.md)。

## I/O 后端

每个 `io_context` 使用一个就绪通知后端。使用 `io_backend::automatic`（默认）时它选择：

| 平台 | 自动选择 | 其他选择 |
|:--|:--|:--|
| Linux | epoll | io_uring，需以 `--uring=y` 构建 |
| macOS、FreeBSD | kqueue | — |
| Windows，64 位 | Windows 提供时用 IOCP 套接字通知（`ProcessSocketNotifications`），否则用 WSAPoll | WSAPoll |
| Windows，32 位 | WSAPoll | — |

环境变量 `PACE_IO_BACKEND` 或显式的构造参数可以选择后端；请求平台不具备的后端会抛出 `std::errc::not_supported`
的 `std::system_error`。选择规则和各后端的限制见 [I/O 后端](runtime/io-backends.zh.md)。

## 仅限 POSIX 的功能

- **QUIC 与 HTTP/3** 需要 POSIX 平台（Linux、macOS、FreeBSD）。为 Windows 或 MinGW 配置 `--quic=y` 或 `--http3=y`
  会在获取任何包之前以配置错误终止。其他一切，包括 TLS、DNS 和 HTTP/2，都能在 Windows 上使用。
- **io_uring** 仅限 Linux；在其他平台上 `--uring=y` 不起作用。
- **Sanitizer**（`--sanitizers=y`）不用于 MSVC。

## 原生套接字句柄

`<pace/runtime/platform.hpp>`（由 `<pace/runtime.hpp>` 包含）声明了可移植的句柄类型；声明见 [`io_context`](runtime/io-context.zh.md)
页面。平台相关的事实：

| | POSIX | Windows |
|:--|:--|:--|
| `pace::native_socket_type` | `int`（文件描述符） | `std::uintptr_t`（`SOCKET`） |
| `pace::invalid_socket` | `-1` | `INVALID_SOCKET`（所有位为 1） |

- Windows 套接字与指针等宽：不要把它存进 `int`，也不要用 POSIX 的 `close` 关闭它。
- `native_handle()` 成员只是出借句柄，从不转移所有权。原始就绪等待需要一个有效的描述符，其所有者负责与 context
  协调取消和关闭。在 POSIX 上任何可轮询的描述符都可以（管道、终端、TUN 设备）；在 Windows 上只适用于 Winsock 套接字，
  `io_context::wait_handle` 则用来等待事件、进程等内核对象（见[句柄与信号](runtime/handles-and-signals.zh.md)）。PACE
  没有可移植的异步文件 API。
- 在 Windows 上，PACE 在创建第一个套接字或轮询器之前初始化 Winsock（`WSAStartup` 2.2，每个进程一次）；应用无需为
  PACE 调用 `WSAStartup`。PACE 创建的套接字是重叠式且不可继承的。
- 在 Windows 上，`<pace/runtime/platform.hpp>` 会包含 `<winsock2.h>` 和 `<ws2tcpip.h>`，并在 `NOMINMAX` 和
  `WIN32_LEAN_AND_MEAN` 尚未定义时定义它们。`pace_runtime` 目标还为依赖它的目标定义 `_WIN32_WINNT=0x0A00`
  （Windows 10），并链接 `ws2_32` 和 `bcrypt`。在你自己的平台相关代码中，先包含 Winsock 头文件，再包含任何会引入
  旧式 `<winsock.h>` 的头文件。

## 参见

- [构建选项](build-options.zh.md)
- [I/O 后端](runtime/io-backends.zh.md)、[运行时](runtime/README.zh.md)、[网络](net/README.zh.md)
- [QUIC](quic/README.zh.md)、[HTTP/3](http3/README.zh.md)
- [CI 覆盖](../contributing/ci.zh.md)、[Windows ARM64 OpenSSL](../contributing/windows-arm64-openssl.zh.md)
- [架构](../explanation/architecture.zh.md)
