<!-- source-hash: 77bab1c8e3a4 -->
# UDP（`pace::udp`）

[English](udp.md) · [网络](README.zh.md)

## 概述

`pace::udp` 提供绑定到 `io_context` 的非阻塞 UDP 套接字、拥有数据的收发辅助函数，以及一次调用完成请求/应答的辅助
函数。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/net/udp.hpp>`（由 `<pace/net.hpp>` 包含） | `pace_net` | 无（始终构建） |

端点是数字形式的 IPv4 或 IPv6 地址；主机名请先用 [DNS 模块](../dns/README.zh.md)解析。UDP 保留消息边界，但不保证
送达、顺序或不重复。启用 `--modules=y` 时，这些名字由 `import pace.net;` 导出。仓库中的示例是
`examples/tutorial_udp.cpp`（`xmake run tutorial_udp`）。

## 概要

```cpp
#include <pace/net.hpp>
#include <pace/runtime.hpp>

#include <iostream>

pace::task<void> hello()
{
    auto receiver = pace::udp::bind({"127.0.0.1", 0});
    auto sender = pace::udp::open();
    co_await sender.send("Hello, UDP!", receiver.local_endpoint());

    auto packet = co_await receiver.receive();
    std::cout << packet.data << '\n';
    co_await receiver.send("reply", packet.sender);
}

int main()
{
    pace::run(hello);
}
```

## 类型

### `endpoint`

```cpp
struct endpoint
{
    std::string address = "127.0.0.1";
    std::uint16_t port = 0;
    address_family family = address_family::unspecified;
    std::uint32_t scope_id = 0;
};
```

字段和规则与 [`tcp::endpoint`](tcp.zh.md#类型) 相同：数字形式、不带方括号的 IPv4 或 IPv6 地址，端口，可选且必须与
地址一致的地址族，以及放在 `scope_id` 中的 IPv6 接口索引（只有 IPv6 才能非零）。主机名、方括号、NUL 字节、格式
错误的地址和 `%zone` 后缀都会抛出 `std::invalid_argument`；应写成 `{"fe80::1", port, pace::address_family::ipv6, 3}`
而不是 `fe80::1%3`。`::1` 是 IPv6 回环地址，`::` 是 IPv6 通配地址。发送方和本地端点报告具体地址族，并把作用域与
地址分开保存。PACE 不解析接口名。

### `bind_options`

```cpp
struct bind_options
{
    bool ipv6_only = true;
};
```

在 IPv6 端点上，无论操作系统默认值如何，都会在绑定前设置 `IPV6_V6ONLY`。`false` 让绑定到 `::` 的一个套接字同时接收
两种地址族，例如 `pace::udp::socket receiver(io, {"::", 0}, {.ipv6_only = false});`。在 IPv4 端点上必须保持
`true`。在这样的双栈套接字上，IPv4 发送方显示为 IPv4 映射的 IPv6 端点（`::ffff:127.0.0.1`），可以直接传回给
`send_to`；`send_to` 也会把普通 IPv4 目标地址映射为这种形式。仅 IPv6 的套接字拒绝 IPv4 目标地址。

### `datagram`

```cpp
struct datagram
{
    std::size_t size = 0;
    endpoint sender;
    bool truncated = false;
};
```

`receive_from` 的结果。`size` 是复制到缓冲区的字节数；`0` 是合法的空数据报，不是 EOF。`sender` 是来源地址和端口。
`truncated` 为 `true` 表示数据报比缓冲区大：剩余部分已丢弃，下一次接收从下一个数据报开始。

### `packet`

```cpp
struct packet
{
    std::string data;
    endpoint sender;
    bool truncated = false;
};
```

`receive` 和 `exchange` 的结果，拥有自己的字节。`data` 为空是合法的空数据报。`truncated` 的含义与 `datagram` 相同。

### `socket`

```cpp
class socket
{
public:
    explicit socket(io_context& context);
    socket(io_context& context, address_family family);
    socket(io_context& context, endpoint local, bind_options options = {});
    ~socket();
    socket(socket&& other) noexcept;
    socket& operator=(socket&& other) noexcept;
    socket(const socket&) = delete;
    socket& operator=(const socket&) = delete;

    void bind(endpoint local, bind_options options = {});
    endpoint local_endpoint() const;
    void set_broadcast(bool enabled = true);
    task<std::size_t> send_to(std::span<const std::byte> bytes, endpoint destination);
    task<datagram> receive_from(std::span<std::byte> buffer);
    task<std::size_t> send(std::string bytes, endpoint destination);
    task<packet> receive(std::size_t capacity = 65536);
    void close() noexcept;

    bool is_open() const noexcept;
    native_socket_type native_handle() const noexcept;
    io_context& context() const noexcept;
};
```

属于某个 `io_context` 的只能移动的 UDP 套接字。

#### 构造与地址族

| 构造函数 | 效果 |
|:--|:--|
| `socket(io)` | 未绑定的套接字。初始为 IPv4 句柄；第一次以 IPv6 地址 `bind` 或 `send_to` 时换成 IPv6 句柄 |
| `socket(io, family)` | 显式地址族的未绑定套接字，保持原句柄并拒绝另一地址族的地址 |
| `socket(io, local, options)` | 创建套接字并绑定到 `local` |

`socket(io)` 的地址族由第一次 `bind`、`send_to`、`receive_from` 或 `set_broadcast` 确定（后两者确定为 IPv4）；之后
再使用另一地址族的地址会抛出错误码为 `address_family_not_supported` 的 `std::system_error`。需要在第一次操作前通过
`native_handle()` 设置原生选项时，请使用显式地址族的构造函数。构造可以在上下文运行之前进行；操作系统失败时抛出
`std::system_error`。

#### `bind`

```cpp
void bind(endpoint local, bind_options options = {});
```

把空闲的套接字绑定到本地地址。端口 `0` 选择一个空闲端口，用 `local_endpoint()` 读回。在有未完成的发送或接收时绑定
会抛出 `std::logic_error`。同步执行，可以在上下文运行之前调用。

#### 发送

```cpp
task<std::size_t> send_to(std::span<const std::byte> bytes, endpoint destination);
task<std::size_t> send(std::string bytes, endpoint destination);
```

发送一个数据报并返回其大小。`send_to` 在完成前借用这些字节；文本写作 `send_to(pace::as_bytes(text), destination)`。
`send` 拥有自己的字符串。空载荷会发送一个空数据报。
载荷超过 65,507 字节，或操作系统只接受了一部分时，抛出错误码为 `std::errc::message_size` 的 `std::system_error`；
路径 MTU 可能使实际可用的上限小得多。完成表示本地套接字接受了该数据报，不表示已经送达。

#### 接收

```cpp
task<datagram> receive_from(std::span<std::byte> buffer);
task<packet> receive(std::size_t capacity = 65536);
```

等待一个数据报。`receive_from` 把它复制到借用的 `buffer` 中，缓冲区不能为空（`std::invalid_argument`）。`receive`
分配 `capacity` 字节，返回只含实际收到字节的 `packet`；`capacity == 0` 时抛出 `std::invalid_argument`，
`capacity > 65536` 时抛出 `std::length_error`，都发生在任何 I/O 之前。容量太小会截断数据报，请检查 `truncated`。
需要在多次接收间复用缓冲区时使用 `receive_from`；字符缓冲区经过 `pace::as_writable_bytes` 传入：

```cpp
std::array<char, 1500> buffer{};
for (;;)
{
    const auto received = co_await socket.receive_from(pace::as_writable_bytes(buffer));
    const std::string_view payload(buffer.data(), received.size);
    co_await socket.send_to(pace::as_bytes(payload), received.sender);
}
```

#### `set_broadcast`

```cpp
void set_broadcast(bool enabled = true);
```

设置或清除 `SO_BROADCAST`。仅限 IPv4：IPv6 没有广播，在 IPv6 套接字上调用会抛出错误码为 `operation_not_supported`
的 `std::system_error`。没有加入组播组的接口。

#### `close` 与访问器

`close()` 取消挂起的等待并关闭句柄；可重复调用，析构函数也会调用它。挂起的操作随后以 `std::system_error` 结束，
因此在它们退出之前保持对象存活。`local_endpoint()` 返回绑定的地址和端口，套接字已关闭时抛出 `std::system_error`。
`is_open()` 报告套接字是否拥有句柄，`native_handle()` 返回借用的原生套接字（不要关闭它或改变它的阻塞模式），
`context()` 返回所属上下文。

## 函数

### `open` 与 `bind`

```cpp
socket open(io_context& io);
socket open();
socket bind(io_context& io, endpoint local, bind_options options = {});
socket bind(endpoint local, bind_options options = {});
```

`open` 返回一个未绑定、首次使用时确定地址族的套接字；`bind` 返回一个已绑定到 `local` 的套接字。两者都是同步的。
不带上下文的重载在调用时就调用 `pace::this_context()`，因此必须在运行中上下文的任务或回调里调用；在 `io.run()`
之前请使用显式传入上下文的重载。

### `exchange` 与 `async_exchange`

```cpp
task<packet> async_exchange(io_context& io, endpoint peer, std::string request, std::size_t capacity = 65536);
task<packet> async_exchange(endpoint peer, std::string request, std::size_t capacity = 65536);
packet exchange(endpoint peer, std::string request, blocking_options options = {}, std::size_t capacity = 65536);
```

从一个与对端地址族相同的新套接字把 `request` 作为一个数据报发出，然后返回第一个来自相同 IP 地址、端口、地址族和
IPv6 作用域的应答（地址规范化后比较）。其他发送方的数据报会被丢弃，且不会延长任何截止时间。这种过滤不是认证，不按
请求 ID 关联，也不重传：请求或应答丢失只能通过截止时间发现。从不同地址或端口应答的服务，以及广播和组播，需要使用
`socket`。

- `capacity` 必须在 1 到 65536 字节之间（为零抛出 `std::invalid_argument`，超出抛出 `std::length_error`），
  `peer.port` 必须非零（`std::invalid_argument`）；两者都在发送之前检查。
- 匹配但被截断的应答以 `truncated = true` 返回，不会重试。空应答是合法的。
- `async_exchange` 在已有上下文上执行（不带 `io` 的重载使用当前运行的上下文），自身不添加截止时间，继承等待它的
  任务的取消和截止时间。没有截止时间时，它会一直等待应答。
- `exchange` 阻塞调用线程，按 [`blocking_options`](../runtime/run.zh.md) 在新建的上下文上执行交换。其 `timeout`
  （默认 30 s）或 `deadline` 约束整个调用；到期时抛出错误码为 `std::errc::timed_out` 的 `std::system_error`。
  `timeout` 不为正数时抛出 `std::invalid_argument`，在 PACE 上下文内调用会在任何 I/O 之前抛出 `std::logic_error`。

### `send`

```cpp
std::size_t send(endpoint peer, std::string message, blocking_options options = {});
```

阻塞调用：在新建的上下文上从一个新套接字发送一个数据报，返回本地套接字接受的字节数。它不等待、也不证明送达。
`blocking_options` 的行为与 `exchange` 相同。在任务中请改用自有套接字上的 `co_await socket.send(message, peer)`。

## 错误

| 情形 | 异常 |
|:--|:--|
| 端点格式错误、主机名、`%zone`、地址族或作用域不符 | `std::invalid_argument` |
| 在 IPv4 端点上 `ipv6_only = false` | `std::invalid_argument` |
| 地址族确定后使用另一地址族的地址 | `std::system_error`（`address_family_not_supported`） |
| `receive_from` 缓冲区为空，`receive`/`exchange` 容量为 0，`exchange` 端口为 0 | `std::invalid_argument` |
| `receive`/`exchange` 容量超过 65536 | `std::length_error` |
| 载荷超过 65,507 字节或只发送了一部分 | `std::system_error`（`message_size`） |
| 在 IPv6 上调用 `set_broadcast` | `std::system_error`（`operation_not_supported`） |
| 已有发送时再次发送、已有接收时再次接收；忙碌时 `bind` | `std::logic_error` |
| 在所属上下文之外执行异步操作 | `std::logic_error` |
| 在已关闭的套接字上操作 | `std::system_error`（`bad_file_descriptor`） |
| 上下文已停止、等待期间套接字被关闭，或任务被取消 | `std::system_error`（`operation_canceled`） |
| 等待方任务的截止时间或 `exchange`/`send` 的时间预算到期 | `std::system_error`（`timed_out`） |
| 其他操作系统错误 | 带原生错误码的 `std::system_error` |

与 TCP 一样，请把 `error.code()` 与 `std::errc` 错误条件比较，而不是比较数值 `errno`。

## 线程与并发

- 异步操作必须在所属上下文上执行；`bind`、`set_broadcast`、`local_endpoint` 和 `close` 还可以在上下文运行之前或
  停止之后调用。要从其他线程关闭套接字，请把调用 `post` 到它的上下文。
- 同一时间可以有一个 `send_to`/`send` 和一个 `receive_from`/`receive` 未完成。同一方向上的第二个操作会立即抛出
  `std::logic_error`。

## 生命周期与所有权

- `io_context` 的寿命必须长于在其上创建的所有套接字。
- 只要有挂起的操作，就保持套接字存活且地址不变；`send_to` 和 `receive_from` 的缓冲区（包括 `pace::as_bytes` 或
  `pace::as_writable_bytes` 视图背后的存储）在操作完成前必须有效。`send`、`receive` 和各自由函数拥有自己的数据。
- 只在套接字空闲时移动它。移动赋值会先关闭目标原有的套接字。

## 限制与默认值

| 项目 | 默认值或上限 |
|:--|:--|
| `endpoint` 默认值 | `"127.0.0.1"`、端口 `0`、`unspecified`、作用域 `0` |
| `bind_options::ipv6_only` | `true` |
| `send_to` 接受的最大载荷 | 65,507 字节 |
| `receive` / `exchange` 的 `capacity` | 默认 65536，允许 1–65536 |
| `exchange` / `send` 超时（`blocking_options::timeout`） | 30 s |
| `async_exchange` 超时 | 无；请使用截止时间 |

## 参见

- [网络概览](README.zh.md)、[TCP](tcp.zh.md)
- 主机名解析用 [DNS](../dns/README.zh.md)，基于 UDP 的可靠流用 [QUIC](../quic/README.zh.md)
- [运行程序](../runtime/run.zh.md)、[结构化并发](../runtime/structured-concurrency.zh.md)
- 教程：[UDP](../../tutorial/06-udp.zh.md)
