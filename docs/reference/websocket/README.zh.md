<!-- source-hash: b95e61b62ce0 -->
# WebSocket

[English](README.md) · [参考](../README.zh.md)

## 概述

`pace::websocket` 提供 RFC 6455 WebSocket 客户端和服务端，运行在 TCP（`ws`）之上，启用 TLS 后也可运行在已验证的
TLS（`wss`）之上：HTTP/1.1 Upgrade 握手、文本与二进制消息、分片、子协议选择、ping/pong 以及关闭握手。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/websocket.hpp>` | `pace_websocket` | `--websocket=y`（默认开启）；`secure_stream` 还需要 `--tls=y` |

本模块定义 `PACE_HAS_WEBSOCKET`。普通 `ws` 不依赖 OpenSSL，也不使用 HTTP/1 模块：WebSocket 流自行解析 Upgrade 请求
或响应。nonce 和掩码密钥来自操作系统的密码学安全随机数生成器。对应的 C++ 命名模块是 `pace.websocket`。

## 概要

```cpp
#include <pace/runtime.hpp>
#include <pace/websocket.hpp>

#include <iostream>

namespace ws = pace::websocket;

pace::task<void> echo(pace::tcp::stream transport)
{
    auto socket = co_await ws::accept(
        std::move(transport),
        {"echo"},
        [](const ws::upgrade_request& request) { return request.target == "/echo"; } // Your Origin/auth policy.
    );
    while (auto message = co_await socket.receive()) // nullopt once the peer has closed.
    {
        co_await socket.send(std::move(*message));
    }
}

int main()
{
    pace::run(
        [](pace::io_context& io) -> pace::task<void>
        {
            pace::tcp::listener listener(io, {"127.0.0.1", 0});
            co_await pace::with_task_group(
                [&](pace::task_group& group) -> pace::task<void>
                {
                    auto server = group.spawn(
                        [&]() -> pace::task<void> { co_await echo(co_await listener.accept()); }
                    );

                    auto socket = co_await ws::connect(listener.local_endpoint(), "localhost", "/echo", {"echo"});
                    co_await socket.send_text("Hello from PACE");
                    if (auto reply = co_await socket.receive())
                    {
                        std::cout << reply->data << " (" << socket.selected_protocol() << ")\n";
                    }
                    co_await socket.close_gracefully();
                    co_await server;
                }
            );
        }
    );
}
```

## 类型

### `role`、`message_type` 与 `message`

```cpp
enum class role { client, server };
enum class message_type { text, binary };

struct message
{
    message_type type = message_type::text;
    std::string data;
};
```

`role` 决定流执行 Upgrade 握手的哪一方。`message` 是由分片重组而成的一条完整应用消息；`data` 可以包含任意字节，
包括 NUL。文本必须是有效的 UTF-8。

### `limits`

```cpp
struct limits
{
    std::size_t max_header_bytes = std::size_t{16} * 1024;
    std::size_t max_frame_bytes = std::size_t{64} * 1024;
    std::size_t max_message_bytes = std::size_t{1024} * 1024;
    std::size_t max_fragments = 1024;
    std::size_t max_control_frames = 1024;
};
```

每个流的上限，传给构造函数或辅助函数。

- `max_header_bytes`：HTTP Upgrade 请求头或响应头。头部解析另外最多接受 100 个字段。
- `max_frame_bytes`：单个数据帧的载荷，收发都适用。
- `max_message_bytes`：一条重组后消息的载荷。
- `max_fragments`：一条消息中的数据帧数。
- `max_control_frames`：一次 `receive()` 调用处理的 ping、pong 和 close 帧数，用于限制控制帧洪泛。

`max_header_bytes` 必须在 64 字节到 1 MiB 之间；其他各项必须大于零，且 `max_frame_bytes` 不得超过
`max_message_bytes`。在分配载荷之前就会检查大小。

### `upgrade_request`

```cpp
struct upgrade_request
{
    std::string target;
    std::string host;
    std::string origin;
    std::vector<std::string> protocols;
};
```

客户端请求的内容，由服务端的 `accept()` 返回并传给其授权回调：请求 target（路径与查询串）、`Host` 和 `Origin`
的值，以及按客户端顺序列出的所提供子协议。不暴露其他请求头。

### `protocol_error`

```cpp
class protocol_error : public std::runtime_error
{
public:
    protocol_error(std::uint16_t code, const std::string& message);
    std::uint16_t close_code() const noexcept;
};
```

WebSocket 协议违规，来自对端或 Upgrade 交换。`close_code()` 是描述它的 RFC 6455 状态码：1002（协议错误）、
1007（无效 UTF-8）、1008（策略：控制帧洪泛，或应用拒绝了 Upgrade）或 1009（消息、帧或分片超限）。

### `basic_stream`、`stream` 与 `secure_stream`

```cpp
template <class Transport>
class basic_stream
{
public:
    explicit basic_stream(Transport transport, role mode, limits bounds = {});
    ~basic_stream();
    basic_stream(basic_stream&&) noexcept;
    basic_stream& operator=(basic_stream&&) noexcept;

    task<void> handshake(std::string host, std::string target = "/", std::vector<std::string> protocols = {});
    task<upgrade_request> accept(std::vector<std::string> supported_protocols = {},
                                 std::function<bool(const upgrade_request&)> authorize = {});

    task<std::optional<message>> receive();
    task<void> send(message value, std::size_t fragment_bytes = 0);
    task<void> send_text(std::string text, std::size_t fragment_bytes = 0);
    task<void> send_binary(std::string bytes, std::size_t fragment_bytes = 0);
    task<void> ping(std::string payload = {});
    task<void> pong(std::string payload = {});
    task<void> send_close(std::uint16_t code = 1000, std::string reason = {});
    task<bool> close_gracefully(std::uint16_t code = 1000, std::string reason = {},
                                std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
    void close() noexcept;

    bool is_open() const noexcept;
    io_context& context() const noexcept;
    std::string selected_protocol() const;
    std::optional<std::uint16_t> peer_close_code() const;
    std::string peer_close_reason() const;
};

using stream = basic_stream<tcp::stream>;
using secure_stream = basic_stream<tls::stream>; // with --tls=y
```

运行在自有传输层之上的 WebSocket：传输层是已连接的 `tcp::stream`，或已完成握手且未协商 ALPN 或协商为
`http/1.1` 的 `tls::stream`（`h2` 会被拒绝：未实现基于 HTTP/2 的 WebSocket）。在从传输层读取任何 HTTP 字节之前
就把它交给 WebSocket：流自行解析 Upgrade，包括与头部一起到达的帧字节。不支持从 HTTP 服务端已解析的请求转换。

- `basic_stream(transport, mode, bounds)` 接管 `transport` 并校验 `bounds`。
- `handshake(host, target, protocols)` 执行客户端 Upgrade。`host` 是 `Host` 的值，与传输层连接的数字地址无关；
  `target` 是以 `/` 开头、不含 fragment 的 origin 形式路径与查询串；`protocols` 是提供的子协议。客户端检查精确的
  accept 密钥、`Connection` 和 `Upgrade` 记号、所选子协议确实被提供过，并且没有协商任何扩展。
- `accept(supported_protocols, authorize)` 执行服务端 Upgrade 并返回 `upgrade_request`。服务端在自己的
  `supported_protocols` 中选出第一个客户端也提供的子协议；没有交集时不选择任何子协议，这是 RFC 6455 允许的，
  如果应用必须使用某个子协议，请检查 `selected_protocol()`。`authorize` 若已设置，会同步执行且不得阻塞；返回
  false 时回应 403。无效请求回应 400，13 以外的 WebSocket 版本回应 426 并附上支持的版本。
- `receive()` 返回下一条完整消息。它重组分片，在重组后校验 UTF-8，回应 ping 并消耗 pong，在一条消息的分片之间
  也是如此。对端关闭后，它发送关闭回复、关闭传输层并返回空 optional。
- `send(value, fragment_bytes)` 以 `fragment_bytes` 大小的帧发送一条消息；0 或大于 `max_frame_bytes` 的值表示
  `max_frame_bytes`。文本会检查 UTF-8。数据在挂起期间由操作持有。`send_text` 和 `send_binary` 是简写。
- `ping(payload)` 和 `pong(payload)` 发送最多 125 字节的控制帧。收到的 ping 由 `receive()` 回应；没有 pong 回调或
  心跳定时器。
- `send_close(code, reason)` 发起关闭握手；之后调用 `receive()` 处理对端的回复。reason 必须是有效 UTF-8，最多
  123 字节。之后不能再发送数据。
- `close_gracefully(code, reason, timeout)` 发送 close（若尚未发送），丢弃收到的消息直到对端的 close 帧到达，并在
  任何情况下都关闭传输层。对端 close 帧到达时返回 true，`timeout` 先到期时返回 false；其他错误会传播。超时是
  协作式的，交换一结束就释放其定时器。
- `close()` 和析构函数立即关闭传输层，不进行关闭握手。
- `selected_protocol()` 是协商出的子协议或空字符串。`peer_close_code()` 和 `peer_close_reason()` 报告对端的 close
  帧；没有收到 close 帧或其中不含状态码时，状态码为空。

## 函数

```cpp
template <class Transport>
task<basic_stream<Transport>> upgrade(Transport transport, std::string authority, std::string target = "/",
                                      std::vector<std::string> protocols = {}, limits bounds = {});
```

客户端：包装已连接的 TCP 或已握手的 TLS `transport`，并执行 `handshake()`。`authority` 是 `Host` 的值，需要时
包含端口，不是 URL。TLS 握手之后用这个形式建立 `wss`。

```cpp
template <class Transport>
task<basic_stream<Transport>> accept(Transport transport, std::vector<std::string> protocols = {},
                                     std::function<bool(const upgrade_request&)> authorize = {},
                                     limits bounds = {});
```

服务端：包装已接受的 TCP 或已握手的 TLS `transport`，并执行 `basic_stream::accept()`。`authorize` 为空时接受所有
请求。它只返回流；如果还需要 `upgrade_request`，请使用成员函数 `accept()`。

```cpp
task<stream> connect(io_context& io, tcp::endpoint peer, std::string authority, std::string target = "/",
                     std::vector<std::string> protocols = {}, limits bounds = {});
task<stream> connect(tcp::endpoint peer, std::string authority, std::string target = "/",
                     std::vector<std::string> protocols = {}, limits bounds = {});
```

客户端：以 TCP 连接到数字地址 `peer`，然后执行 `upgrade()`。第二种形式使用当前 context。

这些辅助函数持有自己的传输层和全部参数；`authorize` 回调按引用捕获的内容仍是借用的。它们不解析 `ws://` URL，
也不解析主机名；需要时请使用 [DNS](../dns/README.zh.md)。

## 错误

| 情况 | 异常 |
|:--|:--|
| 无效的 role 或 limits，无效的 host、target 或子协议列表，Upgrade 请求超过 `max_header_bytes`，无效的发出消息（UTF-8、大小、分片数），控制帧载荷超过 125 字节，无效的关闭码或 reason | `std::invalid_argument` |
| 对端协议违规、无效或被拒绝的 Upgrade、未收到 close 帧就遇到输入结束 | `websocket::protocol_error` |
| 操作重叠、从其他 context 调用、Upgrade 之前的操作、角色不符或重复握手、关闭后发送、使用已被移出的流、TLS 传输层的 ALPN 不是 `http/1.1` | `std::logic_error` |
| 取消，或 context 已停止 | `std::system_error`，`std::errc::operation_canceled` |
| 传输层已关闭 | `std::system_error`，`std::errc::bad_file_descriptor` |
| 套接字和 TLS 错误 | 传输层抛出的 `std::system_error` |

流会拒绝错误的掩码方向、保留位和保留操作码、扩展、非最短长度编码、意外的续帧、分片或超长的控制帧、无效 UTF-8、
无效关闭码以及超限消息。会话打开期间发生协议错误时，它会（尽力）发送相应的 close 帧并关闭传输层。活动操作中的
任何失败都会关闭传输层，只有重叠拒绝例外：它不影响正在运行的操作。

## 线程与并发

- **每个流同一时间只能有一个操作**，发送与接收、握手以及关闭调用都算在内。独立的读协程和写协程不能共享一个流；
  第二个操作会抛出 `std::logic_error`。这种流适合请求/响应和回显式交换，不适合在 `receive()` 挂起期间由服务端
  主动推送。
- 所有成员都在传输层所属的 context 上运行。`close()` 也要在该线程上调用，或在 context 未运行时调用。

## 生命周期与所有权

- 流拥有其传输层，并在销毁时关闭它。在所有操作完成前，保持流及其 context 存活。
- 只在流空闲时移动它。被移出的流只支持析构、赋值、`close()` 和 `is_open()`。
- 发出的数据由操作持有；收到的消息归调用方所有。
- `authorize` 回调在 `accept()` 期间被保存；它按引用捕获的内容必须比 `accept()` 活得更久。

## 限制与默认值

| 设置 | 默认值 | 范围 |
|:--|--:|:--|
| `max_header_bytes` | 16 KiB | 64 字节 – 1 MiB |
| `max_frame_bytes` | 64 KiB | 非零，≤ `max_message_bytes` |
| `max_message_bytes` | 1 MiB | 非零 |
| `max_fragments` | 每消息 1024 | 非零 |
| `max_control_frames` | 每次 `receive()` 1024 | 非零 |
| Upgrade 头部字段 | 100 | 固定 |
| 控制帧载荷 | 125 字节 | 固定 |
| 关闭 reason | 123 字节 UTF-8 | 固定 |
| `close_gracefully()` 超时 | 100 毫秒 | |
| 默认关闭码 | 1000 | |
| 接收缓冲区 | 每流 8 KiB | 固定 |

不提供：`permessage-deflate` 压缩、基于 HTTP/2 的 WebSocket（RFC 8441）、URL 解析、自动重连、部分消息的流式交付
以及同时读写。

## 参见

- `secure_stream` 见 [TLS](../tls/README.zh.md)
- [TCP](../net/tcp.zh.md)、[HTTP/1](../http1/README.zh.md)
- 教程：[WebSocket](../../tutorial/11-websocket.zh.md)
- [可运行示例](../../../examples/tutorial_websocket.cpp)
- [RFC 6455](https://www.rfc-editor.org/rfc/rfc6455)
