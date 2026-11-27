<!-- source-hash: 6c18b8d2be2e -->
# QUIC

[English](README.md) · [参考](../README.zh.md)

## 概述

`pace::quic` 提供 QUIC v1 连接及其相互独立、可多路复用的流，由 TLS 1.3 保护，客户端强制验证对端。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/quic.hpp>` | `pace_quic` | `--quic=y`（`--http3=y` 也会启用） |

本模块有两个可互换的后端，在配置构建时选择。两者实现 `include/pace/quic/connection.hpp` 中同一套 API，
都定义 `PACE_HAS_QUIC`，HTTP/3 在两者之上都能运行。

| `--quic_backend=` | 报文处理、丢包恢复、拥塞控制与流量控制 | TLS 与密码学 | 额外的宏 |
|:--|:--|:--|:--|
| `ngtcp2`（默认） | ngtcp2 1.26.0 | OpenSSL 3.5.7 | `PACE_QUIC_NGTCP2` |
| `openssl` | OpenSSL 3.5.7 原生 QUIC | OpenSSL 3.5.7 | 无 |

QUIC 既不需要 `--tls=y`，也不需要 `pace_tls` 目标：它自带 TLS 1.3 握手。传输层只支持 POSIX（Linux、macOS、
FreeBSD）；在 Windows 或 MinGW 上配置 `--quic=y` 或 `--http3=y` 会以配置错误终止。地址必须是数字形式的 IPv4
或 IPv6 端点；主机名请先用 [DNS](../dns/README.zh.md) 解析。对应的 C++ 命名模块是 `pace.quic`。

## 概要

```cpp
#include <pace/quic.hpp>
#include <pace/runtime.hpp>

#include <array>
#include <iostream>
#include <string>

pace::task<std::string> read_to_fin(pace::quic::stream& stream)
{
    std::array<char, 4096> buffer{};
    std::string bytes;
    while (auto count = co_await stream.read_some(buffer))
    {
        bytes.append(buffer.data(), count);
    }
    co_return bytes;
}

pace::task<void> echo_once(pace::quic::listener& listener)
{
    auto connection = co_await listener.accept();
    auto stream = co_await connection.accept_stream();
    co_await stream.write_all("echo: " + co_await read_to_fin(stream));
    stream.finish_write();
    while (stream.pending_write_bytes() != 0) // Keep the stream until the peer has acknowledged it.
    {
        co_await stream.wait_events();
    }
}

pace::task<void> ask(pace::quic::listener& listener, const pace::quic::context& client_config)
{
    co_await pace::with_task_group(
        [&](pace::task_group& group) -> pace::task<void>
        {
            auto server = group.spawn(echo_once(listener));
            auto connection = co_await pace::quic::connect(client_config, listener.local_endpoint(), "localhost");
            auto stream = co_await connection.open_stream();
            co_await stream.write_all("hello");
            stream.finish_write(); // FIN on our direction; the reply still arrives.
            std::cout << co_await read_to_fin(stream) << '\n';
            co_await server; // Keep the connection open until the server has its acknowledgement.
        }
    );
}

int main()
{
    pace::quic::context server_config(pace::quic::role::server);
    server_config.use_certificate_chain("localhost.pem", "localhost-key.pem");
    server_config.set_alpn_protocols({"example"});

    pace::quic::context client_config; // role::client, system trust store
    client_config.load_verify_file("ca.pem");
    client_config.set_alpn_protocols({"example"});

    pace::run(
        [&](pace::io_context& io) -> pace::task<void>
        {
            auto listener = pace::quic::listen(io, {"127.0.0.1", 0}, server_config);
            co_await ask(listener, client_config);
        }
    );
}
```

## 类型

### `role` 与 `stream_type`

```cpp
enum class role { client, server };
enum class stream_type { bidirectional, unidirectional };
```

`role` 决定一个 `context` 配置的是握手的哪一方。`stream_type` 决定 `connection::open_stream()` 打开的流的方向。

### `context`

```cpp
class context
{
public:
    explicit context(role mode = role::client);
    void load_verify_file(const std::string& path);
    void use_certificate_chain(const std::string& chain, const std::string& key);
    void set_alpn_protocols(const std::vector<std::string>& protocols);
};
```

一方的 QUIC/TLS 1.3 共享配置。复制开销很小，副本共享同一份状态。

- `context(mode)` 为 `mode` 创建配置，TLS 固定为 1.3。客户端加载系统信任库并始终验证服务端；本 API 没有
  不验证证书的客户端模式，也不支持客户端证书（双向 TLS）。初始 ALPN 列表为 `{"pace-quic"}`。
- `load_verify_file(path)` 把 `path` 中的 PEM 证书加入受信根证书，系统信任库仍然有效。
- `use_certificate_chain(chain, key)` 为服务端加载 PEM 证书链及其私钥。私钥必须是未加密的 PEM 文件，加载时
  从不提示输入口令。与证书不匹配的私钥会被拒绝。
- `set_alpn_protocols(protocols)` 替换 ALPN 列表。每个标识符 1–255 字节，编码后的列表最多 65535 字节；空列表
  会被拒绝。服务端按自己列表的顺序选出第一个客户端也提供的协议；没有交集时握手失败。用
  `connection::selected_protocol()` 读取结果。

在第一个 `connection` 或 `listener` 使用之前完成 context 的全部配置，并且在它们存活期间不要再修改。

### `connection`

```cpp
class connection
{
public:
    connection(io_context& io, const quic::context& config);
    connection(connection&&) noexcept;
    connection& operator=(connection&&) noexcept;
    ~connection();

    task<void> connect(udp::endpoint peer, std::string expected_name,
                       std::chrono::milliseconds timeout = std::chrono::seconds(10));
    task<stream> open_stream(stream_type type = stream_type::bidirectional);
    std::optional<stream> try_open_stream(stream_type type = stream_type::bidirectional);
    task<stream> accept_stream();
    std::optional<stream> try_accept_stream();
    task<void> wait_events();
    io_context& context() const noexcept;
    std::string selected_protocol() const;
    bool is_open() const noexcept;
    void close(std::uint64_t application_error = 0) noexcept;
};
```

客户端连接，或由 `listener::accept()` 返回的连接。只能移动。

- `connection(io, config)` 在 `io` 上创建一个尚未连接的客户端，绑定到临时 UDP 端口。`config` 必须是客户端
  context。
- `connect(peer, expected_name, timeout)` 与 `peer` 完成握手，并按 `expected_name` 验证服务端证书；该名称与
  网络地址无关。DNS 名称同时作为 SNI 发送，并须匹配证书的 DNS 主题备用名称；IPv4 或 IPv6 字面量须匹配 IP
  主题备用名称。空名称、含 NUL 的名称和端口 0 会被拒绝。`timeout` 限制握手时长；非正值只允许立即尝试一次。
  `connect()` 失败后连接即被关闭。
- `open_stream(type)` 打开一个流，在达到对端的流数量上限时等待。打开流本身不发送任何数据：对端在收到它的
  第一批数据或 FIN 时才看到这个流。
- `accept_stream()` 等待对端打开的下一个流。流在其第一批数据或 FIN 到达时才可见；不同流的到达顺序不构成
  顺序保证。
- `try_open_stream(type)` 和 `try_accept_stream()` 做同样的事但不挂起；无法打开流或没有等待中的流时返回空
  optional。
- `wait_events()` 挂起直到连接上有传输进展。唤醒可能是虚假的：之后要重新检查每个相关的流。它供使用 `try_*`
  成员多路复用大量流的协议驱动使用。
- `selected_protocol()` 返回协商出的 ALPN 协议；握手完成前返回空字符串。
- `close()` 之后、连接失败之后以及所属 context 停止之后，`is_open()` 为 false。
- `close(application_error)` 立即尽力发送带 `application_error` 的 CONNECTION_CLOSE。它丢弃尚未确认的输出，
  使该连接的所有流失效并唤醒它们挂起的操作。没有优雅排空。关闭一个已接受的连接不影响同一监听器上的其他连接。

### `stream`

```cpp
class stream
{
public:
    stream(stream&&) noexcept;
    stream& operator=(stream&&) noexcept;
    ~stream();

    std::uint64_t id() const noexcept;
    bool is_unidirectional() const noexcept;
    bool is_local() const noexcept;
    io_context& context() const noexcept;
    task<std::size_t> read_some(std::span<char> buffer);
    task<void> write_all(std::string_view bytes);
    task<std::size_t> read_some(std::span<std::byte> buffer);
    task<void> write_all(std::span<const std::byte> bytes);
    task<void> wait_events();
    std::optional<std::size_t> try_read(std::span<char> buffer);
    std::size_t try_write(std::string_view bytes);
    std::uint64_t pending_write_bytes() const;
    void finish_write();
    void reset(std::uint64_t application_error = 0) noexcept;
};
```

连接中的一个流。只能移动。流句柄会让其连接的状态保持存活。

- `id()` 是 QUIC 流 ID；`is_unidirectional()` 和 `is_local()`（由本端打开）由它推导。`context()` 是所属的
  `io_context`。
- `read_some(buffer)` 等待至少一个字节并返回读到的字节数。只有在对端 FIN 时，或缓冲区为空时立即返回 0。流被
  重置或连接失败是错误，而不是返回 0。
- `write_all(bytes)` 在传输层接受了全部字节后完成，必要时施加流量控制背压。这并不表示对端已收到或确认。字节
  是借用的：在任务完成前保持其有效。
- `std::byte` 重载的行为相同。
- `try_read(buffer)` 不挂起地读取：空 optional 表示暂无数据，0 表示 FIN，否则是复制的字节数。
- `try_write(bytes)` 不挂起地按流量控制窗口所允许的量入队，并返回入队字节数；对非空输入返回 0 表示会阻塞。
  在有进展后重试剩余部分。
- `wait_events()` 挂起直到连接上有传输进展；唤醒可能是虚假的。
- `pending_write_bytes()` 是已写入但对端尚未确认的字节数。用写操作累计接受的字节数减去它，在两个后端中都得到
  已确认的前缀。它不包括 FIN，而且被确认不代表应用已读取数据。
- `finish_write()` 在发送方向上排入 FIN，接收方向仍可使用。
- `reset(application_error)` 以 `application_error` 放弃该流，并唤醒其挂起的操作。

读取单向流的只发送端、写入其只接收端以及在 `finish_write()` 之后写入都是错误。

### `listener`

```cpp
class listener
{
public:
    listener(io_context& io, udp::endpoint local, const quic::context& config);
    ~listener();

    task<connection> accept(std::chrono::milliseconds handshake_timeout = std::chrono::seconds(10));
    udp::endpoint local_endpoint() const;
    io_context& context() const noexcept;
    bool is_open() const noexcept;
    void close() noexcept;
};
```

接受 QUIC 连接的已绑定 UDP 套接字。不可复制，也不可移动。

- `listener(io, local, config)` 立即绑定 `local` 并开始接收。`config` 必须是带证书的服务端 context。IPv6 端点
  以仅 IPv6 方式绑定。
- `accept(handshake_timeout)` 从接受队列取出下一个连接并完成其握手。超时从连接离开队列时开始计时，不限制等待
  新对端的时间。握手失败（证书、ALPN 或截止时间）会抛出异常，但只要 `is_open()` 为 true，监听器就继续接受连接。
- `local_endpoint()` 返回绑定的地址；端口为 0 时返回系统选定的端口。
- `close()` 之后、引擎失败之后以及所属 context 停止之后，`is_open()` 为 false。
- `close()` 取消挂起的 `accept()`，并使该监听器上接受的所有连接失效。析构函数会调用它。

## 函数

```cpp
listener listen(io_context& io, udp::endpoint local, const quic::context& configuration);
listener listen(udp::endpoint local, const quic::context& configuration);
```

绑定一个监听器，与 `listener` 构造函数完全相同。第二种形式使用当前 context，必须在运行中的 `io_context` 内调用。

```cpp
task<connection> connect(io_context& io, quic::context configuration, udp::endpoint peer,
                         std::string expected_name,
                         std::chrono::milliseconds timeout = std::chrono::seconds(10));
task<connection> connect(quic::context configuration, udp::endpoint peer, std::string expected_name,
                         std::chrono::milliseconds timeout = std::chrono::seconds(10));
```

创建客户端 `connection` 并完成 `connection::connect()`。任务持有 `configuration` 的副本（一个共享句柄），调用方
的对象无需比它活得更久。超时只限制握手，不限制之后的流操作。第二种形式使用当前 context。ALPN 和证书身份仍需
显式给出，库不会替你选择。

[`<pace/net/stream_io.hpp>`](../net/README.zh.md#函数) 中的流辅助函数（`pace::write`、`pace::read_exact`）同样接受
`quic::stream`。

## 错误

| 情况 | 异常 |
|:--|:--|
| 无效参数：role、空的或过大的 ALPN、空的或含 NUL 的名称、端口 0、context 角色不符、流类型 | `std::invalid_argument` |
| `openssl` 后端下带作用域的 IPv6 目标地址 | `std::invalid_argument` |
| 同类操作重叠、从其他 context 调用、握手前操作、流方向错误、FIN 之后写入 | `std::logic_error` |
| 握手截止时间到达 | `std::system_error`，`std::errc::timed_out` |
| 连接、监听器或 context 已关闭或停止；取消 | `std::system_error`，`std::errc::operation_canceled` |
| 对端关闭连接、空闲超时 | `std::system_error`，`std::errc::connection_aborted` |
| TLS 或 QUIC 失败：证书不受信或不匹配、没有共同 ALPN、私钥或信任文件错误、协议违规 | `std::system_error`，`std::errc::protocol_error`；消息中带有后端的诊断信息 |
| UDP 套接字失败 | 带系统错误码的 `std::system_error` |

共享 UDP 引擎内部的失败会关闭该引擎，并报告给其上的每个操作。服务端应在 `accept()` 和每个连接的处理周围捕获
单个对端的错误，以免一个对端影响无关的任务。

## 线程与并发

- 所有成员（包括 `try_*` 成员）都在运行所属 `io_context` 的线程上执行；从其他地方调用会抛出
  `std::logic_error`。句柄的构造和析构也应在该线程上进行，或在 context 未运行时进行。
- 每个流同时最多有一个读和一个写。每个连接同时最多有一个 `connect()`、一个打开和一个接受操作。每个监听器同时
  最多一个 `accept()`。同类的第二个操作会抛出 `std::logic_error`。
- 不同的流相互独立地并发推进。一个监听器的所有连接以及一个连接的所有流共享同一个 UDP 套接字；`wait_events()`
  在其上任何地方有进展时都会唤醒，因此每次唤醒后都要重新检查状态。
- 两个后端都由 PACE 的 UDP 就绪通知和可取消的 `io_context` 定时器驱动，没有轮询线程。库回调从不直接恢复
  应用代码：后续执行会排入 context 的队列。

## 生命周期与所有权

- `io_context` 必须比其上的所有监听器、连接和流活得更久。
- `stream` 持有其连接状态的共享引用，连接持有其 UDP 引擎的共享引用。因此销毁 `connection` 对象不会使其流失效。
  传输层随最后一个句柄释放；如果需要保证送达，在释放前等待 `pending_write_bytes()` 变为 0，并保持连接打开直到
  对端完成。
- `connection::close()` 使该连接的所有流失效。`listener::close()` 和监听器的析构函数使其上接受的所有连接失效，
  因此在这些连接使用期间要保持监听器存活。
- `write_all()` 和 `read_some()` 借用缓冲区，直到任务完成。
- 由 `context` 创建的连接和监听器共享它，并使其保持存活。

## 限制与默认值

| 设置 | 值 |
|:--|:--|
| 握手超时（`connect`、`accept`） | 10 秒；非正值只允许立即尝试一次 |
| 初始 ALPN 列表 | `{"pace-quic"}` |
| ALPN 标识符 / 编码后的列表 | 1–255 字节 / 最多 65535 字节 |
| TLS 版本 | 仅 1.3 |
| 客户端信任 | 系统信任库加上 `load_verify_file()` |
| IPv6 监听器 | 仅 IPv6 |
| 带作用域的 IPv6 目标地址 | 保留（`ngtcp2`）；拒绝（`openssl`） |

`ngtcp2` 后端使用以下传输设置。`openssl` 后端使用 OpenSSL 的默认值。

| `ngtcp2` 设置 | 值 |
|:--|:--|
| UDP 载荷大小 | 1200 字节；关闭 PMTU 发现 |
| 每流流量控制窗口 | 256 KiB |
| 连接流量控制窗口 | 4 MiB |
| 同时暴露的入站流 | 每个方向 64 个；句柄释放后归还额度 |
| 每次 `try_write()` | 最多 16 KiB |
| 待接受队列 | 128 个连接 |
| 未完成的服务端握手 | 10 秒后过期，即使没有调用 `accept()` |
| 空闲超时 | 30 秒 |
| 每批接收或发送的报文数 | 64 |
| 主动连接迁移 | 禁用 |

不提供：0-RTT、会话恢复控制、连接迁移、QUIC DATAGRAM、自定义拥塞控制、客户端证书以及应用层优雅关闭。

## 参见

- 运行在本模块之上的 [HTTP/3](../http3/README.zh.md)
- `udp::endpoint` 见 [UDP](../net/udp.zh.md)
- 连接前解析名称见 [DNS](../dns/README.zh.md)
- [平台支持](../platform-support.zh.md)与[构建选项](../build-options.zh.md)
- 教程：[QUIC 与 HTTP/3](../../tutorial/13-quic-and-http3.zh.md)
- [可运行示例](../../../examples/tutorial_quic.cpp)
- [RFC 9000](https://www.rfc-editor.org/rfc/rfc9000)、[ngtcp2 programmer's guide](https://nghttp2.org/ngtcp2/programmers-guide.html)、
  [OpenSSL QUIC](https://docs.openssl.org/3.5/man7/openssl-quic/)
