# Platform support

[简体中文](platform-support.zh.md) · [Reference](README.md)

## Overview

PACE runs on Linux, macOS, FreeBSD and Windows. Every module is available on all four except QUIC and HTTP/3,
which need a POSIX platform. This page lists the supported platforms and compilers, what CI checks on each, the
I/O backend each one uses, and the platform-specific types and behavior of the public API.

| Platform | Architectures run in CI | Modules | I/O backends |
|:--|:--|:--|:--|
| Linux | x86_64, aarch64, i386 (32-bit process) | All | epoll (default), io_uring (`--uring=y`) |
| macOS | x86_64, arm64 | All | kqueue |
| FreeBSD | x86_64 | All | kqueue |
| Windows (MSVC) | x64, x86, arm64 | All except QUIC and HTTP/3 | IOCP socket notifications, WSAPoll |
| Windows (MinGW) | x86_64 | Core, HTTP/1 and WebSocket tested; QUIC and HTTP/3 unsupported | IOCP socket notifications, WSAPoll |

## Compilers

PACE is C++20 (C++23 and later can be selected with `--cxxstandard`). The supported minimum compilers are:

| Compiler | Minimum |
|:--|:--|
| GCC | 12 |
| Clang | 16 |
| Apple Clang | 15 |
| MSVC | 19.38 (Visual Studio 2022 17.8) |

CI does not build with these minimum versions. It builds and tests with the compilers of its runners:

| CI job | Compiler |
|:--|:--|
| Linux x86_64, aarch64, i386; C++20 and C++23 | GCC of Ubuntu 24.04 (GCC 13) |
| Linux x86_64 | Clang of Ubuntu 24.04 (Clang 18) |
| QUIC backends, both backends with every module | GCC 15, GCC 16.2, Clang 21 |
| C++ named modules | Clang 18 (module jobs), GCC 16 and Clang 21 (QUIC backend jobs) |
| macOS 15, x86_64 and arm64 | Apple Clang of the runner's default Xcode |
| FreeBSD 14.3 | the system Clang |
| Windows Server 2025 and Windows 11 ARM | MSVC of the runner's Visual Studio 2022 |
| Windows MinGW | GCC of MSYS2 UCRT64 |
| Linux armv7, riscv64, ppc64le, s390x (emulated) | Ubuntu 24.04 cross GCC |
| Android, iOS (compile only) | NDK 27 Clang, Xcode Clang |

C++ named modules (`--modules=y`) additionally need Clang or GCC 16; see [build options](build-options.md).

## What CI checks

The [CI coverage guide](../contributing/ci.md) lists every job; in short:

- **Native execution, every module**: Linux x86_64 (GCC and Clang, C++20 and C++23, plus an io_uring job),
  Linux aarch64, Linux i386, macOS x86_64 and arm64, FreeBSD x86_64. Each builds, runs every test and tutorial, and
  runs independent Python WebSocket peers.
- **Native execution, every module except QUIC and HTTP/3**: Windows x64 (IOCP), x86 (WSAPoll) and arm64 (IOCP)
  with MSVC. These jobs also check that configuring QUIC fails with the expected error.
- **Native execution, core, HTTP/1 and WebSocket**: Windows x86_64 with MinGW.
- **Emulated execution, core, HTTP/1 and WebSocket**: Linux armv7, riscv64, ppc64le and s390x under QEMU user mode.
  These run on the host kernel and say nothing about native hardware or performance.
- **Compile only, core, HTTP/1 and WebSocket**: Android (armeabi-v7a, arm64-v8a, x86_64; API 30) and iOS arm64
  (device and simulator). Networking, application integration and device restrictions on these platforms are
  unverified.
- **Linux only**: AddressSanitizer, UndefinedBehaviorSanitizer and leak checks with every module; each module
  built and tested on its own with the [header check](build-options.md#header-self-containment-check); both QUIC
  backends exchanging data with each other.

Supported here means "built and tested as listed"; it does not imply equal performance. Published performance
numbers come from local Linux measurements; see [performance](../performance/README.md).

## I/O backends

Each `io_context` uses one readiness backend. With `io_backend::automatic` (the default) it picks:

| Platform | Automatic choice | Other choices |
|:--|:--|:--|
| Linux | epoll | io_uring, when built with `--uring=y` |
| macOS, FreeBSD | kqueue | — |
| Windows, 64-bit | IOCP socket notifications (`ProcessSocketNotifications`) where Windows provides them, otherwise WSAPoll | WSAPoll |
| Windows, 32-bit | WSAPoll | — |

The `PACE_IO_BACKEND` environment variable or an explicit constructor argument selects a backend; asking for one
the platform does not have throws `std::system_error` with `std::errc::not_supported`. See
[I/O backends](runtime/io-backends.md) for the selection rules and each backend's limits.

## POSIX-only features

- **QUIC and HTTP/3** need a POSIX platform (Linux, macOS, FreeBSD). Configuring `--quic=y` or `--http3=y` for
  Windows or MinGW stops with a configuration error before any package is fetched. Everything else, including TLS,
  DNS and HTTP/2, works on Windows.
- **io_uring** is Linux only; `--uring=y` does nothing elsewhere.
- **Sanitizers** (`--sanitizers=y`) are not applied with MSVC.

## Native socket handles

`<pace/runtime/platform.hpp>` (included by `<pace/runtime.hpp>`) declares the portable handle type; the
declarations are on the [`io_context`](runtime/io-context.md) page. The platform facts:

| | POSIX | Windows |
|:--|:--|:--|
| `pace::native_socket_type` | `int` (a file descriptor) | `std::uintptr_t` (a `SOCKET`) |
| `pace::invalid_socket` | `-1` | `INVALID_SOCKET` (all bits set) |

- A Windows socket is pointer-sized: do not store it in an `int` or close it with POSIX `close`.
- `native_handle()` members lend the handle; they never transfer ownership. Raw readiness waits need a valid
  descriptor whose owner coordinates cancellation and closing with the context. On POSIX any pollable descriptor
  works (pipes, terminals, TUN devices); on Windows only Winsock sockets do, and `io_context::wait_handle` waits for
  kernel objects such as events and processes (see [Handles and signals](runtime/handles-and-signals.md)). PACE has
  no portable asynchronous file API.
- On Windows, PACE initializes Winsock (`WSAStartup` 2.2, once per process) before it creates its first socket or
  poller; applications do not call `WSAStartup` for PACE. PACE creates overlapped, non-inheritable sockets.
- On Windows, `<pace/runtime/platform.hpp>` includes `<winsock2.h>` and `<ws2tcpip.h>` and defines `NOMINMAX` and
  `WIN32_LEAN_AND_MEAN` if they are not defined yet. The `pace_runtime` target also defines `_WIN32_WINNT=0x0A00`
  (Windows 10) and links `ws2_32` and `bcrypt` for its dependents. In your own platform code, include Winsock
  headers before any header that pulls in the legacy `<winsock.h>`.

## See also

- [Build options](build-options.md)
- [I/O backends](runtime/io-backends.md), [runtime](runtime/README.md), [networking](net/README.md)
- [QUIC](quic/README.md), [HTTP/3](http3/README.md)
- [CI coverage](../contributing/ci.md), [Windows ARM64 OpenSSL](../contributing/windows-arm64-openssl.md)
- [Architecture](../explanation/architecture.md)
