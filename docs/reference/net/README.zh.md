<!-- source-hash: 14c2170ffad1 -->
# 网络（`pace/net.hpp`）

[English](README.md) · [参考手册](../README.zh.md)

## 概述

net 模块在 PACE 反应器之上提供非阻塞的 TCP 与 UDP 套接字、TCP、TLS 与 QUIC 流共同满足的 `pace::byte_stream`
概念、适用于任何字节流的通用辅助函数，以及文本与字节之间的零拷贝视图。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/net.hpp>` | `pace_net` | 无（始终构建） |

`<pace/net.hpp>` 包含本模块的全部头文件：

| 头文件 | 内容 | 页面 |
|:--|:--|:--|
| `<pace/net/ip.hpp>` | `pace::address_family` | 本页 |
| `<pace/net/buffer.hpp>` | `pace::as_bytes`、`pace::as_writable_bytes`、`pace::as_text` | 本页 |
| `<pace/net/stream_io.hpp>` | `pace::byte_stream`；面向字节流的 `pace::write`、`pace::read_exact` | 本页 |
| `<pace/net/tcp.hpp>` | `pace::tcp`：`endpoint`、`stream`、`listener`、`connect`、`listen`、`exchange` | [TCP](tcp.zh.md) |
| `<pace/net/udp.hpp>` | `pace::udp`：`endpoint`、`socket`、`datagram`、`packet`、`open`、`bind`、`exchange`、`send` | [UDP](udp.zh.md) |

`pace_net` 依赖 `pace_runtime`，除 C++ 标准库和操作系统的套接字 API 之外没有其他依赖。启用 `--modules=y` 时，
`import pace.net;` 导出这些页面中的名字；`io_context`、`task`、`run` 还需要 `import pace.runtime;`，或者直接用
`import pace;` 导入全部。

本模块所有页面共同遵守的规则：

- 地址是数字形式的 IPv4 或 IPv6 字符串。本模块不解析主机名，请先使用 [DNS 模块](../dns/README.zh.md)。
- 每个套接字属于一个 `io_context`：构造时传入的那个（不带 `io_context&` 参数的重载则使用当前正在运行的那个）。
  它的异步操作在该上下文的线程上执行。
- 操作返回惰性的 [`task`](../runtime/task.zh.md)：只有被 `co_await` 或 `spawn` 之后才会执行。
- 套接字只能移动、不能复制，且只能在没有未完成操作时移动。
- 流和套接字以 `std::byte` 读写：`read_some(std::span<std::byte>)` 与 `write_all(std::span<const std::byte>)`，
  UDP 数据报也使用同样的 span 类型。文本通过 [`<pace/net/buffer.hpp>`](#as_bytesas_writable_bytes-与-as_text)
  的视图跨过这道边界，不产生拷贝：`write_all(pace::as_bytes("text"))`、`read_some(pace::as_writable_bytes(buffer))`。
- TCP 和 UDP 热路径接受 `pace::as_result` 并以错误码报告失败。默认重载抛出异常。见[错误处理](../errors.zh.md#以结果值返回错误)。

## 客户端选项

TCP、UDP、TLS 客户端辅助函数共用 `<pace/net/client_options.hpp>` 中的 `pace::client_options`。例如 `udp::async_exchange(peer, request, {.context = &io, .timeout = 2s, .max_reply_size = 512})`。

- 协程在开始执行时读取运行中的 context；显式的 `context` 必须匹配。阻塞入口拥有新建的 context，传入非空 `context` 会抛出 `std::invalid_argument`。
- `timeout` 必须为正数。协程默认继承外层期限，阻塞调用默认 30 秒。相对超时、绝对 `deadline` 和外层期限中最早者生效；`cancellation` 可取消整个操作。
- `max_reply_size` 限制应答分配。TCP/TLS 默认 1 MiB，UDP 默认 65536 字节。`tls` 指向借用的 TLS 配置。`transport` 将套接字设置和只移动的 TCP 连接钩子传给 TCP/TLS。具名选项通过 `std::move(options)` 交给一次调用。

## 概要

```cpp
#include <pace/net.hpp>
#include <pace/runtime.hpp>

#include <array>
#include <iostream>
#include <string_view>

pace::task<void> demo(pace::io_context& io)
{
    auto listener = pace::tcp::listen(io, {"127.0.0.1", 0});
    auto client = co_await pace::tcp::connect(listener.local_endpoint(), {.context = &io});
    auto server = co_await listener.accept();

    co_await pace::write(client, "ping"); // 拥有自己的字符串
    std::cout << co_await pace::read_exact(server, 4) << '\n';

    co_await server.write_all(pace::as_bytes("pong")); // 借用这些字节
    std::array<char, 64> buffer{};
    const auto count = co_await client.read_some(pace::as_writable_bytes(buffer));
    std::cout << std::string_view(buffer.data(), count) << '\n';
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

### `byte_stream`

```cpp
// <pace/net/stream_io.hpp>
namespace pace
{
template <class Stream>
concept byte_stream =
    requires(Stream& stream, const Stream& owner, std::span<std::byte> input, std::span<const std::byte> output) {
        { stream.read_some(input) } -> std::same_as<task<std::size_t>>;
        { stream.write_all(output) } -> std::same_as<task<void>>;
        { stream.shutdown_write() } -> std::same_as<task<void>>;
        { stream.close() } noexcept;
        { owner.context() } noexcept -> std::same_as<io_context&>;
    };
}
```

属于某个 `io_context` 的已连接、有序、双向字节流。[`tcp::stream`](tcp.zh.md#stream)、
[`tls::stream`](../tls/README.zh.md#stream) 和 [`quic::stream`](../quic/README.zh.md#stream) 都满足它，应用自己的
类型也可以满足。[`pace::write`](#write)、[`pace::read_exact`](#read_exact) 以及
[`tls::stream` 的通用构造函数](../tls/README.zh.md#自定义传输层)接受任何 `byte_stream`。`udp::socket` 不是
字节流：它传输的是数据报，而不是流。

概念只检查签名。满足它的类型还要遵守以下契约：

| 成员 | 契约 |
|:--|:--|
| `read_some(buffer)` | 以 1 到 `buffer.size()` 个字节完成；只有在流结束时（以及缓冲区为空时）才以 0 完成 |
| `write_all(bytes)` | 在传输层接收了全部字节后完成，而不是在对端收到之后 |
| `shutdown_write()` | 结束本方向：对端读到流结束，本端仍然可以读 |
| `close()` | 立即释放流；挂起的操作以 `std::errc::operation_canceled` 恢复 |
| `context()` | 执行该流全部操作的 `io_context` |

同一时间可以有一个读和一个写未完成，除非该类型规定了更严格的限制：`tls::stream` 总共只允许一个操作。缓冲区在操作
完成前一直被借用。

| 类型 | `shutdown_write()` | `close()` |
|:--|:--|:--|
| `tcp::stream` | 发送 FIN | 关闭套接字 |
| `tls::stream` | 发送 `close_notify`；读取持续到对端的 `close_notify` | 关闭传输层，不发送 `close_notify` |
| `quic::stream` | 排队 FIN，与 `finish_write()` 相同 | 重置两个方向，与 `reset()` 相同 |

应用类型只要转发给自己拥有的流即可满足该概念，例如统计收到的字节数：

```cpp
#include <pace/net.hpp>
#include <pace/runtime.hpp>

#include <cstddef>
#include <span>
#include <utility>

class counting_stream
{
public:
    explicit counting_stream(pace::tcp::stream inner)
        : inner_(std::move(inner))
    {
    }

    pace::task<std::size_t> read_some(std::span<std::byte> buffer)
    {
        const auto count = co_await inner_.read_some(buffer);
        received_ += count;
        co_return count;
    }

    pace::task<void> write_all(std::span<const std::byte> bytes)
    {
        return inner_.write_all(bytes);
    }

    pace::task<void> shutdown_write()
    {
        return inner_.shutdown_write();
    }

    void close() noexcept
    {
        inner_.close();
    }

    pace::io_context& context() const noexcept
    {
        return inner_.context();
    }

    std::size_t received() const noexcept
    {
        return received_;
    }

private:
    pace::tcp::stream inner_;
    std::size_t received_ = 0;
};

static_assert(pace::byte_stream<counting_stream>);
```

## 函数

### `as_bytes`、`as_writable_bytes` 与 `as_text`

```cpp
// <pace/net/buffer.hpp>
namespace pace
{
std::span<const std::byte> as_bytes(std::string_view text) noexcept;
std::span<std::byte> as_writable_bytes(std::span<char> characters) noexcept;
std::span<std::byte> as_writable_bytes(std::string& text) noexcept;
std::string_view as_text(std::span<const std::byte> bytes) noexcept;
}
```

文本与流读写的字节之间的视图。它们都不拷贝也不分配内存：结果指向实参的存储，只在该存储有效期间有效。

| 函数 | 结果 |
|:--|:--|
| `as_bytes(text)` | 把 `text` 的字符看作字节，供 `write_all` 和 `send_to` 使用；接受字符串字面量、`std::string` 或 `std::string_view` |
| `as_writable_bytes(characters)` | 把字符缓冲区（`std::array<char, N>`、`std::vector<char>`、`char` 数组）看作可写字节，供 `read_some` 和 `receive_from` 使用 |
| `as_writable_bytes(text)` | 把已设好大小的 `std::string` 的字符看作可写字节，用于读入其中；字符串大小不变 |
| `as_text(bytes)` | 把字节看作 `std::string_view`，用于解析读到的内容 |

```cpp
co_await stream.write_all(pace::as_bytes("GET / HTTP/1.0\r\n\r\n"));

std::string reply(512, '\0');
reply.resize(co_await stream.read_some(pace::as_writable_bytes(reply)));
```

传给 `as_bytes` 的临时 `std::string` 一直存活到完整表达式结束，因此
`co_await stream.write_all(pace::as_bytes(prefix + body))` 是安全的；但保存在变量里的视图不能比它的字符串活得久。
与 `std::as_bytes` 不同，`pace::as_bytes` 接受的是文本而不是 span。已经存放 `std::byte` 的缓冲区（例如
`std::array<std::byte, N>`）直接传入即可。

### `write`

```cpp
template <byte_stream Stream>
task<void> write(Stream& stream, std::string bytes);
```

用 `stream.write_all` 写出 `bytes` 的全部字节。任务拥有这个字符串，因此传入临时对象、或在任务执行前就离开作用域的
字符串都是安全的：左值参数会被复制，右值会被移动。字符串可以包含二进制数据，包括零字节。完成只表示字节已交给
传输层，不表示对端已收到或处理。`write_all` 抛出的异常原样传播。

### `read_exact`

```cpp
template <byte_stream Stream>
task<void> read_exact(Stream& stream, std::span<std::byte> bytes);
template <byte_stream Stream>
task<std::string> read_exact(Stream& stream, std::size_t size, std::size_t maximum = std::size_t{1024} * 1024);
```

反复调用 `read_some`，直到读满所请求的字节数。它从不越过请求的长度读取，因此下一条消息的字节仍留在流中。请求长度
为零时不读取，直接完成。

- span 重载填充调用方的缓冲区，该缓冲区在任务完成前必须保持有效。操作失败时，缓冲区的某个前缀可能已被改写。字符
  缓冲区请以 `pace::as_writable_bytes(buffer)` 传入。
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

`as_bytes`、`as_writable_bytes` 和 `as_text` 都是 `noexcept`，不会失败。

## 线程与并发

这些辅助函数在流所属的上下文上执行，并沿用该流的并发规则。`read_exact` 在整个执行期间算作一个读操作：完成之前不要
在同一个流上启动其他读操作。在 `tls::stream` 上则不要启动任何其他操作，因为 TLS 同一时间只允许一个操作。`write`
算作一个写操作。`<pace/net/buffer.hpp>` 的视图不做 I/O，可以在任何线程上使用。

## 生命周期与所有权

流是借用的：任务完成之前，流必须存活、地址不变，并留在其所属上下文上。`write` 拥有它的字节；`read_exact` 的
span 重载借用调用方的缓冲区。`<pace/net/buffer.hpp>` 的视图借用它们所指向的存储：在所有使用该视图的操作完成之前，
该存储必须存活且不被修改。

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
- [迁移到字节接口](../../migration/0.6-to-1.0.zh.md#字节流只有一套字节接口)

## 双向转发

`co_await pace::relay(a, b, options)` 在两个流共同的 context 上双向转发。TCP 流、QUIC 双向流和已完成握手的 TLS 流可以任意配对。转发期间，relay 独占 TLS 会话。自定义传输须允许一次读和一次写同时进行。收到 EOF 时，relay 向另一端发送 TCP FIN、QUIC FIN 或 TLS `close_notify`，对端仍可回传数据。两个方向均结束后返回字节数 `a_to_b` 和 `b_to_a`。运行中出错、取消或超时会关闭两端。

`relay_options::idle_timeout` 限制无进展时间，`linger` 限制第一次 EOF 后等待反向结束的时间。两者默认为零，表示禁用。第四个参数可以是观察者，其 `before_write(direction, size)` 返回 `task<void>`，可在写入前限速。两个普通 TCP 流在 Linux 上可用 `.splice = true` 请求零拷贝，使用观察者时走复制路径。
