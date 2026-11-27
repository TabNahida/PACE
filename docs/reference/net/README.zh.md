<!-- source-hash: 024a7e8567f0 -->
# 网络（`pace/net.hpp`）

[English](README.md) · [参考手册](../README.zh.md)

## 概述

net 模块在 PACE 反应器之上提供非阻塞的 TCP 与 UDP 套接字，以及适用于任何 PACE 字节流（TCP、TLS、QUIC 流）的通用辅助函数。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/net.hpp>` | `pace_net` | 无（始终构建） |

`<pace/net.hpp>` 包含本模块的全部头文件：

| 头文件 | 内容 | 页面 |
|:--|:--|:--|
| `<pace/net/ip.hpp>` | `pace::address_family` | 本页 |
| `<pace/net/stream_io.hpp>` | 面向字节流的 `pace::write`、`pace::read_exact` | 本页 |
| `<pace/net/tcp.hpp>` | `pace::tcp`：`endpoint`、`stream`、`listener`、`connect`、`listen`、`exchange` | [TCP](tcp.zh.md) |
| `<pace/net/udp.hpp>` | `pace::udp`：`endpoint`、`socket`、`datagram`、`packet`、`open`、`bind`、`exchange`、`send` | [UDP](udp.zh.md) |
| `<pace/net/aliases.hpp>` | 已弃用的别名（`tcp_stream` 等） | 本页 |

`pace_net` 依赖 `pace_runtime`，除 C++ 标准库和操作系统的套接字 API 之外没有其他依赖。启用 `--modules=y` 时，
`import pace.net;` 导出这些页面中的名字；`io_context`、`task`、`run` 还需要 `import pace.runtime;`，或者直接用
`import pace;` 导入全部。

本模块所有页面共同遵守的规则：

- 地址是数字形式的 IPv4 或 IPv6 字符串。本模块不解析主机名，请先使用 [DNS 模块](../dns/README.zh.md)。
- 每个套接字属于一个 `io_context`：构造时传入的那个（不带 `io_context&` 参数的重载则使用当前正在运行的那个）。
  它的异步操作在该上下文的线程上执行。
- 操作返回惰性的 [`task`](../runtime/task.zh.md)：只有被 `co_await` 或 `spawn` 之后才会执行。
- 套接字只能移动、不能复制，且只能在没有未完成操作时移动。
- 错误以异常报告，大多是 `std::system_error`；参见各页的“错误”一节和[错误处理](../errors.zh.md)。

## 概要

```cpp
#include <pace/net.hpp>
#include <pace/runtime.hpp>

#include <iostream>

pace::task<void> demo(pace::io_context& io)
{
    auto listener = pace::tcp::listen(io, {"127.0.0.1", 0});
    auto client = co_await pace::tcp::connect(io, listener.local_endpoint());
    auto server = co_await listener.accept();

    co_await pace::write(client, "ping");
    auto received = co_await pace::read_exact(server, 4);
    std::cout << received << '\n';
}

int main()
{
    pace::run(demo);
}
```

## 类型

### `address_family`

```cpp
// <pace/net/ip.hpp>
namespace pace
{
enum class address_family
{
    unspecified,
    ipv4,
    ipv6
};
}
```

TCP、UDP 端点或套接字的地址族。

| 取值 | 作为输入（端点、套接字构造函数） | 作为输出（查询端点） |
|:--|:--|:--|
| `unspecified` | 由数字地址推断地址族；套接字在首次使用时确定 | 从不返回 |
| `ipv4` | 地址必须是 IPv4 | IPv4 套接字地址 |
| `ipv6` | 地址必须是 IPv6（`::ffff:127.0.0.1` 这类 IPv4 映射地址算作 IPv6） | IPv6 套接字地址 |

显式指定的地址族与地址不符时抛出 `std::invalid_argument`。`local_endpoint()`、`remote_endpoint()` 和收到的数据报
发送方总是报告原生套接字地址的具体地址族。

### 已弃用的别名

```cpp
// <pace/net/aliases.hpp>
namespace pace
{
using tcp_stream = tcp::stream;
using tcp_listener = tcp::listener;
using ipv4_endpoint = tcp::endpoint;
using listen_options = tcp::listen_options;
}
```

这些名字已弃用，将被移除。新代码请使用 `pace::tcp` 中的名字。`ipv4_endpoint` 就是完整的 `tcp::endpoint`，也接受
IPv6。

## 函数

`<pace/net/stream_io.hpp>` 中的辅助函数适用于任何具有 PACE 字节流成员
`read_some(std::span<char>) -> task<std::size_t>` 和 `write_all(std::string_view) -> task<void>` 的类型：
[`tcp::stream`](tcp.zh.md)、[`tls::stream`](../tls/README.zh.md) 和 [`quic::stream`](../quic/README.zh.md)。

### `write`

```cpp
template <class Stream>
task<void> write(Stream& stream, std::string bytes);
```

用 `stream.write_all` 写出 `bytes` 的全部字节。任务拥有这个字符串，因此传入临时对象、或在任务执行前就离开作用域的
字符串都是安全的：左值参数会被复制，右值会被移动。字符串可以包含二进制数据，包括零字节。完成只表示字节已交给
传输层，不表示对端已收到或处理。`write_all` 抛出的异常原样传播。

### `read_exact`

```cpp
template <class Stream>
task<void> read_exact(Stream& stream, std::span<char> bytes);
template <class Stream>
task<void> read_exact(Stream& stream, std::span<std::byte> bytes);
template <class Stream>
task<std::string> read_exact(Stream& stream, std::size_t size, std::size_t maximum = std::size_t{1024} * 1024);
```

反复调用 `read_some`，直到读满所请求的字节数。它从不越过请求的长度读取，因此下一条消息的字节仍留在流中。请求长度
为零时不读取，直接完成。

- span 重载填充调用方的缓冲区，该缓冲区在任务完成前必须保持有效。操作失败时，缓冲区的某个前缀可能已被改写。
- `size` 重载返回一个恰好 `size` 字节、自有数据的字符串。当 `size > maximum` 时，它在分配内存和读取之前抛出
  `std::length_error`。`maximum` 限制的是分配大小而不是流；消息更大时请调高它。

`read_exact` 不带超时，也不做任何分帧：下一条消息有多少字节由调用方决定（例如来自长度前缀）。如需限制等待时间，
请使用[截止时间](../runtime/structured-concurrency.zh.md)。

## 错误

| 情形 | 异常 |
|:--|:--|
| 读满之前遇到 EOF（`read_exact`） | `std::system_error`，错误码 `std::errc::connection_reset` |
| `size > maximum`（`read_exact`） | `std::length_error`，发生在任何 I/O 之前 |
| 传输错误、取消、流已关闭 | 由 `read_some` / `write_all` 原样传播 |

## 线程与并发

这些辅助函数在流所属的上下文上执行，并沿用该流的并发规则。`read_exact` 在整个执行期间算作一个读操作：完成之前不要
在同一个流上启动其他读操作。在 `tls::stream` 上则不要启动任何其他操作，因为 TLS 同一时间只允许一个操作。`write`
算作一个写操作。

## 生命周期与所有权

流是借用的：任务完成之前，流必须存活、地址不变，并留在其所属上下文上。`write` 拥有它的字节；`read_exact` 的
span 重载借用调用方的缓冲区。

## 限制与默认值

| 项目 | 值 |
|:--|:--|
| `read_exact` 的 `maximum` 默认值 | 1 MiB（`1024 * 1024` 字节） |

## 参见

- [TCP](tcp.zh.md)、[UDP](udp.zh.md)
- [TLS](../tls/README.zh.md)、[DNS](../dns/README.zh.md)、[QUIC](../quic/README.zh.md)
- [任务](../runtime/task.zh.md)、[运行程序](../runtime/run.zh.md)、[结构化并发](../runtime/structured-concurrency.zh.md)
- 教程：[TCP](../../tutorial/05-tcp.zh.md)、[UDP](../../tutorial/06-udp.zh.md)
- 指南：[协议调优](../../guides/protocol-tuning.zh.md)
