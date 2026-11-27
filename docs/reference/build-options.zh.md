<!-- source-hash: 8913c1d0b0bc -->
# 构建选项

[English](build-options.md) · [参考](README.zh.md)

## 概述

PACE 使用 [xmake](https://xmake.io) 2.9.8 或更新版本构建（CI 使用 3.0.9 和 3.1.1）。选项通过 `xmake f` 设置，并一直
保留到下一次 `xmake f`；`-c` 会先清除之前的配置。

```sh
xmake f -c -m release --tls=y --http2=y -y
xmake -y
xmake test -v
```

构建由以下文件描述：

| 文件 | 内容 |
|:--|:--|
| [`xmake.lua`](../../xmake.lua) | 项目、语言标准、Windows QUIC 检查、sanitizer 与 fuzz 编译标志，以及对下列文件的引入 |
| [`xmake/options.lua`](../../xmake/options.lua) | 库的选项 |
| [`xmake/packages.lua`](../../xmake/packages.lua) | 固定版本的第三方包 |
| [`xmake/targets.lua`](../../xmake/targets.lua) | 每个模块一个库目标，以及聚合目标 `pace` |
| [`xmake/modules.lua`](../../xmake/modules.lua) | C++ 命名模块目标 `pace_module` |
| [`xmake/checks.lua`](../../xmake/checks.lua) | 头文件自包含检查 |
| [`xmake/tests.lua`](../../xmake/tests.lua)、[`examples.lua`](../../xmake/examples.lua)、[`benchmarks.lua`](../../xmake/benchmarks.lua)、[`fuzz.lua`](../../xmake/fuzz.lua) | 测试、示例、基准测试和 fuzzer，各自带有自己的选项 |

构建模式为 `debug`、`release` 和 `releasedbg`（`-m`）。运行时与网络模块不需要任何第三方库；每个可选模块只引入
它自己的包。

## 库选项

| 选项 | 默认值 | 作用 |
|:--|:--|:--|
| `--http1=y/n` | `y` | HTTP/1.0 与 HTTP/1.1 模块，包括 `http::app` 和网关服务（`pace_http1`） |
| `--websocket=y/n` | `y` | WebSocket 模块（`pace_websocket`） |
| `--tls=y/n` | `n` | 基于 OpenSSL 的 TLS 与 HTTPS（`pace_tls`）；同时启用 HTTP/1、HTTP/2 和 WebSocket 的 TLS 形式 |
| `--dns=y/n` | `n` | 基于 c-ares 的异步 DNS（`pace_dns`）；URL 客户端随之能解析主机名 |
| `--quic=y/n` | `n` | QUIC（`pace_quic`）；仅限 POSIX |
| `--quic_backend=ngtcp2/openssl` | `ngtcp2` | QUIC 实现：ngtcp2 配合 OpenSSL 的 TLS，或 OpenSSL 原生 QUIC |
| `--http2=y/n` | `n` | 基于 nghttp2 的 HTTP/2（`pace_http2`）；HTTPS 还需要 `--tls=y` |
| `--http3=y/n` | `n` | 基于 nghttp3 的 HTTP/3（`pace_http3`）；会启用 QUIC |
| `--cxxstandard=cxx20/cxx23/cxxlatest` | `cxx20` | C++ 语言标准 |
| `--modules=y/n` | `n` | 构建 C++ 命名模块（`pace_module`） |
| `--sanitizers=y/n` | `n` | AddressSanitizer 与 UndefinedBehaviorSanitizer |
| `--profiling=y/n` | `n` | 侵入式的按线程阶段计时，仅用于诊断 |
| `--uring=y/n` | `n` | Linux io_uring 就绪后端（liburing） |
| `--fuzz=none/libfuzzer/replay` | `none` | fuzz 目标 |
| `--header_check=y/n` | `n` | 单独编译每个公共头文件 |

每个启用的模块会为所有依赖它的目标定义 `PACE_HAS_<MODULE>`（`PACE_HAS_HTTP1`、`PACE_HAS_WEBSOCKET`、
`PACE_HAS_TLS`、`PACE_HAS_DNS`、`PACE_HAS_QUIC`、`PACE_HAS_HTTP2`、`PACE_HAS_HTTP3`）；ngtcp2 后端还定义
`PACE_QUIC_NGTCP2`。不要自己定义这些宏：它们必须来自库的构建。

在 Windows 或 MinGW 上配置 `--quic=y` 或 `--http3=y` 会在获取任何包之前以错误“QUIC/HTTP3 currently require a
POSIX platform”终止。见[平台支持](platform-support.zh.md)。

只构建运行时和网络时，关闭默认开启的模块：

```sh
xmake f -c -m release --http1=n --websocket=n -y
```

构建全部模块：

```sh
xmake f -c -m release --tls=y --dns=y --quic=y --http2=y --http3=y -y
```

### 包

某个选项第一次需要时，xmake 会获取以下固定版本，可能需要网络访问：

| 包 | 版本 | 使用者 |
|:--|:--|:--|
| OpenSSL | 3.5.7 | `--tls`、`--quic`、`--http3` |
| ngtcp2 | 1.26.0 | `--quic_backend=ngtcp2` 时的 `--quic` / `--http3` |
| nghttp2 | 1.70.0 | `--http2` |
| nghttp3 | 1.18.0 | `--http3` |
| c-ares | 1.34.8 | `--dns` |
| liburing | 2.12 | Linux 上的 `--uring` |
| doctest | 2.5.3 | `--tests`（仅测试程序） |
| Asio、libuv | 1.34.2、1.52.1 | `--benchmarks`（仅对比基准测试） |

Windows ARM64 和 FreeBSD 分别为 OpenSSL 和 ngtcp2 使用仓库内的包定义（`xmake/openssl_arm64.lua`、
`xmake/ngtcp2_bsd.lua`）；见 [Windows ARM64 OpenSSL](../contributing/windows-arm64-openssl.zh.md)。

## 目标

| 目标 | 类型 | 内容 | 依赖 | 包 |
|:--|:--|:--|:--|:--|
| `pace_runtime` | static | 任务、`io_context`、定时器、取消、结构化并发、工作线程 | — | `--uring` 时为 liburing |
| `pace_net` | static | TCP、UDP、地址、流辅助函数 | `pace_runtime` | — |
| `pace_http` | header-only | HTTP 消息与路由器 | `pace_runtime` | — |
| `pace_tls` | static | TLS 流 | `pace_net` | OpenSSL |
| `pace_dns` | static | DNS 解析器 | `pace_net` | c-ares |
| `pace_quic` | static | QUIC（一个后端） | `pace_net` | OpenSSL，该后端需要时加 ngtcp2 |
| `pace_http1` | static | HTTP/1、`http::app`、网关 | `pace_net`、`pace_http`，启用时加 `pace_tls`、`pace_dns` | — |
| `pace_http2` | static | HTTP/2 | `pace_net`、`pace_http`，启用时加 `pace_tls`、`pace_dns` | nghttp2 |
| `pace_http3` | static | HTTP/3 | `pace_net`、`pace_http`、`pace_quic`，启用时加 `pace_dns` | nghttp3 |
| `pace_websocket` | static | WebSocket | `pace_net`，启用时加 `pace_tls` | — |
| `pace` | phony | 所有启用的模块 | 上述已启用的全部目标 | — |
| `pace_module` | static | C++ 命名模块（`--modules=y` 时） | `pace` | — |

模块目标只编译 `src/<module>/`，只安装自己的头文件（`include/pace/<module>/` 和总头文件 `<pace/<module>.hpp>`），
只链接它需要的模块和包。`xmake build pace_tls` 只构建运行时、网络和 TLS。选择 `pace_net` 永远不会链接 OpenSSL、
c-ares 或 HTTP 代码，即使同一项目中的其他目标启用了它们。不存在单一的 `libpace.a`：手写的链接命令必须列出所用的
每个模块及其依赖。

依赖是公开的：依赖某个模块的目标会继承它的包含路径、宏定义、包和系统库（POSIX 上为 `pthread`；Windows 上为
`ws2_32` 和 `bcrypt`）。

## 只依赖一个模块

在本仓库中，或在引入本仓库的项目中，依赖你所用的最窄的目标，并包含其总头文件：

```lua
target("my_application")
    set_kind("binary")
    add_files("main.cpp")
    add_deps("pace_http2")       -- or "pace" for every enabled module
```

```cpp
#include <pace/http2.hpp>        // or <pace/pace.hpp> for every enabled module
```

配置时必须启用该模块（此处为 `--http2=y`）。要在另一个 xmake 项目中构建 PACE，把它的检出放在你的源码树中并引入
其根文件；根文件会加载自己的构建文件，并让路径始终相对于 PACE 的检出目录：

```lua
set_project("my_pace_application")
set_languages("cxx20")
add_rules("mode.debug", "mode.release")

includes("third_party/PACE")

target("my_application")
    set_kind("binary")
    add_files("main.cpp")
    add_deps("pace")
```

只需要库时配置 `--tests=n --examples=n`。库是 C++ ABI 代码，不是稳定的 C ABI：请用与应用相同的工具链和选项
（包括 profiling）构建它。

### 独立的配置

xmake 为每个项目目录保存一份配置。`-o <dir>` 只改变输出位置。要并存多份配置，为每份配置指定自己的
`XMAKE_CONFIGDIR`，并在每条命令中使用它：

```sh
export XMAKE_CONFIGDIR="$PWD/build/core-config"
xmake f -c -m release -o build/core --http1=n --websocket=n -y
xmake -y
xmake test -v
```

取消设置 `XMAKE_CONFIGDIR` 即回到默认配置。

## C++ 命名模块

`--modules=y` 构建 `pace_module`，它为每个库模块提供一个命名模块：`pace.runtime`、`pace.net`、`pace.http`、
`pace.http1`、`pace.gateway`、`pace.tls`、`pace.dns`、`pace.quic`、`pace.http2`、`pace.http3`、`pace.websocket`，
以及包含全部模块的 `pace`。每个模块只导出自己的名称，因此使用 TCP 的程序要同时导入两者：

```cpp
import pace.runtime;
import pace.net;
```

`import pace;` 相当于 `#include <pace/pace.hpp>`。对应库被禁用的模块仍然存在，只是不导出任何名称。`modules/` 中的
模块接口由 `scripts/generate_modules.py` 根据 `modules/exports.json` 生成。它们包装普通的头文件和库，因此模块使用者
和头文件使用者共享同一 ABI，可以在同一程序中混用。

依赖 `pace_module` 而不是 `pace`。命名模块需要 Clang（CI 使用 Clang 18 和 21）或 GCC 16 及更新版本；GCC 15 及更早
版本会报错终止，原因是编译器在模块中处理协程时的缺陷。头文件仍是主要接口；默认构建从不运行模块扫描器。

## 头文件自包含检查

`--header_check=y` 增加 `pace_header_check` 目标，它在独立的编译单元中编译 `include/pace/` 下的每个头文件。这样，
依赖了自己没有包含的头文件的头文件会在这里失败，而不是在使用者那里失败。被禁用模块的头文件编译为空。启用全部
模块来构建它：

```sh
xmake f -c -m release -o build/hc --tls=y --dns=y --quic=y --http2=y --http3=y --header_check=y -y
xmake -y
```

CI 还会在启用该检查的情况下单独构建每个模块，以发现悄悄依赖其他模块的模块。

## Sanitizer

`--sanitizers=y` 让所有目标（包括库本身）以 `-fsanitize=address,undefined -fno-omit-frame-pointer` 编译和链接。
在 `windows`（MSVC）平台上不起作用。请使用 debug 构建，并让失败成为致命错误：

```sh
xmake f -c -m debug -o build/asan --sanitizers=y -y
xmake -y
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 xmake test -v
```

## Profiling

`--profiling=y` 为 `pace_runtime` 及所有依赖它的目标定义 `PACE_ENABLE_PROFILING`，并记录按线程的阶段计时。它会
带来开销：请使用单独的构建目录，做性能对比时保持关闭。应用必须与库使用相同的设置构建。

## io_uring

在 Linux 上，`--uring=y` 把 liburing 链接进 `pace_runtime` 并定义 `PACE_HAS_IO_URING`。在其他平台上该选项不起作用。
启用它不会改变默认后端，默认仍是 epoll：用 `PACE_IO_BACKEND=io_uring` 或显式的 `io_context` 构造参数选择
io_uring。见 [I/O 后端](runtime/io-backends.zh.md)。

## Fuzzing

`--fuzz=libfuzzer`（Clang 与 libFuzzer）为 HTTP/1 服务端和客户端解析器以及 WebSocket 服务端构建 fuzz 目标，并同时
用 ASan 和 UBSan 插桩库代码。`--fuzz=replay` 用一个回放输入文件的小驱动程序构建相同的目标，因此任何工具链都能把
语料库作为回归测试运行。两种模式下 `xmake test` 都会回放 `tests/fuzz/corpus/` 中的种子语料。

```sh
xmake f -c -m debug -o build/fuzz --toolchain=clang --fuzz=libfuzzer -y
xmake -y
xmake run pace_fuzz_http1_server tests/fuzz/corpus/http1_server
```

## 测试、示例与基准测试

| 选项 | 默认值 | 构建内容 |
|:--|:--|:--|
| `--tests=y/n` | `y` | 已启用模块的 doctest 测试程序（`pace_*_tests`），以及供外部互操作对端使用的 `pace_interop_server` |
| `--examples=y/n` | `y` | `echo_server`、`http_server` 以及已启用模块的 `tutorial_*` 程序；`--tests=y` 时教程程序也作为测试运行 |
| `--benchmarks=y/n` | `n` | 与独立 Asio 和 libuv 的 TCP 对比（`pace_bench`） |
| `--components=y/n` | `n` | PACE 组件微基准测试 |
| `--scaling=y/n` | `n` | 工作线程数与执行模式的扩展性基准测试 |
| `--protocol_benchmarks=y/n` | `n` | 已启用模块的 HTTP、WebSocket 和协议探测程序 |

`xmake test -v` 运行所有已构建的测试程序和教程。基准测试方法见[性能](../performance/README.zh.md)。

## 参见

- [平台支持](platform-support.zh.md)
- [I/O 后端](runtime/io-backends.zh.md)
- [安装与第一个程序](../tutorial/01-install-and-first-program.zh.md)
- [CI 覆盖](../contributing/ci.zh.md)
