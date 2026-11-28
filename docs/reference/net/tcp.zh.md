<!-- source-hash: 77a5ca0b208a -->
# TCP（`pace::tcp`）

[English](tcp.md) · [网络](README.zh.md)

## 概述

`pace::tcp` 提供绑定到 `io_context` 的非阻塞 TCP 客户端与服务器套接字，以及一次调用完成请求/应答的辅助函数。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/net/tcp.hpp>`（由 `<pace/net.hpp>` 包含） | `pace_net` | 无（始终构建） |

端点是数字形式的 IPv4 或 IPv6 地址，不会隐式解析主机名。`tcp::stream` 是一个
[`pace::byte_stream`](README.zh.md#byte_stream)：它以 `std::byte` span 读写，因此文本要经过
[`pace::as_bytes` 和 `pace::as_writable_bytes`](README.zh.md#as_bytesas_writable_bytes-与-as_text)，通用的
[`pace::write` 和 `pace::read_exact`](README.zh.md#函数) 也可用于它。启用 `--modules=y` 时，这些名字由
`import pace.net;` 导出。

## 原生套接字与连接选项

`tcp::stream::adopt(io, fd)` 接管空闲的已连接或尚未连接的 TCP 套接字，`tcp::listener::adopt(io, fd)` 接管已监听的套接字。成功后对象独占句柄，失败时由调用方关闭。接管会设置非阻塞和禁止继承标记。不要传入另一个对象的借用句柄或已注册到其他事件循环的句柄。

`tcp::connect_options::socket` 和 `tcp::listen_options::socket` 共用 `pace::socket_options`。可指定接口索引、mark、FreeBSD 路由表、keepalive 及其空闲时间、间隔、次数、TCP 用户超时、DF、Fast Open 和 Linux 透明绑定。未设置的字段保留系统默认值。平台缺少所选能力时会报告错误。Linux 客户端开启 Fast Open 后，首次写入可能才会发起握手。

`pre_connect(fd)` 在地址族选择和选项设置完毕后、系统连接调用前同步执行。钩子抛出的异常会中止这次尝试。`original_destination()` 查询 netfilter 或 PF 的 NAT 映射，PF 需要能够打开 `/dev/pf`。Linux TPROXY 保留目标地址，可直接读取 `local_endpoint()`。

## 概要

```cpp
#include <pace/net.hpp>
#include <pace/runtime.hpp>

#include <array>
#include <cstddef>
#include <iostream>
#include <span>

pace::task<void> echo(pace::tcp::stream peer)
{
    std::array<std::byte, 4096> buffer{};
    while (const auto count = co_await peer.read_some(buffer))
    {
        co_await peer.write_all(std::span(buffer).first(count));
    }
}

pace::task<void> demo(pace::io_context& io)
{
    auto listener = pace::tcp::listen(io, {"127.0.0.1", 0});
    auto client = co_await pace::tcp::connect(listener.local_endpoint(), {.context = &io});
    client.set_no_delay();
    io.spawn(echo(co_await listener.accept()));

    co_await pace::write(client, "Hello, TCP!");
    std::cout << co_await pace::read_exact(client, 11) << '\n';
    co_await client.close_gracefully();
}

int main()
{
    pace::run(demo);
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

| 成员 | 含义 |
|:--|:--|
| `address` | 数字形式、不带方括号的 IPv4 或 IPv6 地址，例如 `127.0.0.1`、`::1` 或 `::ffff:192.0.2.1` |
| `port` | TCP 端口。监听器上的 `0` 表示由操作系统分配空闲端口 |
| `family` | `unspecified` 由 `address` 推断地址族；`ipv4` 或 `ipv6` 必须与地址一致 |
| `scope_id` | 带作用域（链路本地）IPv6 地址的接口索引；IPv4 时必须为 `0` |

多数情况下写 `{address, port}` 即可。主机名、方括号、内嵌 NUL 字节、格式错误的地址、与地址不符的地址族，以及 IPv4
上的非零作用域，都会在端点被使用时抛出 `std::invalid_argument`。`fe80::1%eth0`、`fe80::1%3` 这类文本形式的区域后缀
会被拒绝，应写成 `{"fe80::1", port, pace::address_family::ipv6, 3}`。接口索引的选择以及链路本地地址是否可达，由应用
自行负责。

`local_endpoint()` 和 `remote_endpoint()` 返回的端点包含自有的数字地址、实际端口、具体地址族（从不为
`unspecified`）以及作用域。

### `listen_options`

```cpp
struct listen_options
{
    bool reuse_port = false;
    int backlog = 1024;
    bool ipv6_only = true;
    pace::socket_options socket{};
};
```

| 成员 | 含义 |
|:--|:--|
| `reuse_port` | 请求原生的同端口监听共享：有 `SO_REUSEPORT_LB` 时用它，否则用 `SO_REUSEPORT`。两者都不存在时（Windows）抛出错误码为 `std::errc::operation_not_supported` 的 `std::system_error` |
| `backlog` | 传给 `listen()` 的待处理连接队列长度。必须为正数；操作系统可能会截断 |
| `ipv6_only` | 在 IPv6 端点上设置 `IPV6_V6ONLY`。`false` 让一个通配监听器同时接受 IPv4 和 IPv6。IPv4 端点上必须保持 `true` |

`reuse_port` 允许多个监听器（例如每个工作上下文一个）在同一端口上接受连接，由内核把新连接分摊给它们。它不会在上下文
之间迁移已建立的连接。与该选项无关，PACE 在 POSIX 监听器上总是设置 `SO_REUSEADDR`，在 Windows 上设置
`SO_EXCLUSIVEADDRUSE`。

无论操作系统默认值如何，IPv6 监听器总是显式设置 `IPV6_V6ONLY`，因此另一个 IPv4 监听器可以绑定同一端口。使用
`ipv6_only = false`（例如 `listen(io, {"::", port}, {.ipv6_only = false})`）时，PACE 在绑定前设置 `IPV6_V6ONLY=0`，
失败时报告错误而不会退回其他策略。这种监听器的 IPv4 对端以 IPv4 映射的 IPv6 地址（`::ffff:127.0.0.1`）报告；访问
控制和日志代码必须处理这种形式。

### `stream`

```cpp
class stream
{
public:
    explicit stream(io_context& context);
    stream(io_context& context, address_family family);
    ~stream();
    stream(stream&& other) noexcept;
    stream& operator=(stream&& other) noexcept;
    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;

    task<void> connect(endpoint endpoint, connect_options options = {});
    task<std::size_t> read_some(std::span<std::byte> buffer);
    task<void> read_exact(std::span<std::byte> buffer);
    task<std::string> read_exact(std::size_t size, std::size_t maximum = std::size_t{1024} * 1024);
    task<void> write_all(std::span<const std::byte> bytes);
    task<void> shutdown_write();

    void set_no_delay(bool enabled = true);
    task<void> close_gracefully(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(100),
        std::size_t discard_limit = std::size_t{64} * 1024
    );
    void close() noexcept;

    [[nodiscard]] native_socket_type native_handle() const noexcept;
    [[nodiscard]] io_context& context() const noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] endpoint local_endpoint() const;
    [[nodiscard]] endpoint remote_endpoint() const;
};
```

属于某个 `io_context` 的只能移动的 TCP 连接。流来自 `tcp::connect`、`listener::accept`，或者先构造再 `connect`。

#### 构造

`stream(io)` 创建一个未连接的非阻塞 IPv4 套接字，并在第一次 `connect` 时确定地址族：目标是 IPv6 时，它会换成 IPv6
套接字，并保留 `set_no_delay` 的设置。`stream(io, family)` 传入 `ipv4` 或 `ipv6` 时保持原句柄，拒绝另一地址族的
目标；需要在连接前通过 `native_handle()` 设置其他原生选项时使用它。出站 IPv6 套接字设置 `IPV6_V6ONLY=0`，因此 IPv4
映射的目标地址在所有平台上都可用。构造可以在上下文运行之前进行，操作系统无法创建套接字时抛出 `std::system_error`。

#### `connect`

```cpp
task<void> connect(endpoint endpoint, connect_options options = {});
```

连接到一个数字端点。在流上启动其他任何操作之前先等待它完成。连接结果通过 `SO_ERROR` 检查；被拒绝或其他失败抛出
`std::system_error`。一个流只允许一次连接尝试：原生连接一旦开始，再次 `connect` 会抛出 `std::logic_error`，所以失败
后请关闭该流、改用新流。在校验阶段（原生尝试之前）被拒绝的端点不影响该流，可以换一个正确的端点重试。没有连接超时，
请使用[截止时间](../runtime/structured-concurrency.zh.md)。

#### 读取

```cpp
task<std::size_t> read_some(std::span<std::byte> buffer);
```

等待至少一个字节可读，返回复制的字节数，可能少于 `buffer.size()`。缓冲区非空时，`0` 表示对端关闭了发送方向（有序
EOF）。缓冲区为空时不读取，直接返回 `0`。读到的字节不以 NUL 结尾。读入字符缓冲区时经过 `pace::as_writable_bytes`：

```cpp
std::array<char, 4096> buffer{};
const auto count = co_await stream.read_some(pace::as_writable_bytes(buffer));
std::string_view received(buffer.data(), count);
```

```cpp
task<void> read_exact(std::span<std::byte> buffer);
task<std::string> read_exact(std::size_t size, std::size_t maximum = std::size_t{1024} * 1024);
```

行为与自由函数 [`pace::read_exact`](README.zh.md#read_exact) 相同的成员函数：恰好读满请求的长度而不消费后续字节；
读满之前遇到 EOF 时抛出错误码为 `std::errc::connection_reset` 的 `std::system_error`；`size` 重载在
`size > maximum` 时于分配之前抛出 `std::length_error`。span 读取失败时，缓冲区的某个前缀可能已被改写。

#### 写入

```cpp
task<void> write_all(std::span<const std::byte> bytes);
```

循环处理部分发送，发送整个 span，在操作系统接受了全部字节后完成。这并不表示对端已收到或处理。任务借用这些字节：
完成之前保持它们存活且不变，或者改用拥有字符串的 [`pace::write`](README.zh.md#write)。文本写作
`write_all(pace::as_bytes(text))`。空 span 不发送，直接完成。大块写入每发送 1 MiB 就让出一次反应器，使其他连接得以
推进。在 POSIX 上发送从不触发 `SIGPIPE`；对端已关闭时报告为 `std::system_error`。

#### `shutdown_write`

```cpp
task<void> shutdown_write();
```

半关闭连接：发送 FIN（关闭发送方向的 `shutdown`），对端由此读到 EOF，本端则继续读取，直到对端自己的 EOF。它立即
完成，不等待对端；操作系统失败或流已关闭时抛出 `std::system_error`。不要在有写操作未完成时调用它；之后再调用
`write_all` 会以操作系统错误失败。与 `close_gracefully` 不同，它既不排空输入也不关闭套接字：读完应答后再 `close()`
或销毁流。

```cpp
co_await stream.write_all(pace::as_bytes(request));
co_await stream.shutdown_write(); // 对端在请求之后读到 EOF
std::string reply;
std::array<char, 4096> buffer{};
while (const auto count = co_await stream.read_some(pace::as_writable_bytes(buffer)))
{
    reply.append(buffer.data(), count);
}
```

#### `set_no_delay`

```cpp
void set_no_delay(bool enabled = true);
```

设置（`true`）或清除（`false`）`TCP_NODELAY`。在上下文运行之前或在所属上下文上调用。流已关闭、上下文已停止或操作
系统失败时抛出 `std::system_error`。

#### 有界优雅关闭

```cpp
task<void> close_gracefully(
    std::chrono::milliseconds timeout = std::chrono::milliseconds(100),
    std::size_t discard_limit = std::size_t{64} * 1024
);
```

先半关闭发送方向，再读取并丢弃传入的字节，直到对端 EOF、达到 `timeout` 或丢弃了 `discard_limit`
字节（以先到者为准），然后关闭套接字。这样即使对端仍有未读的请求字节在途，最后的响应也能送达，否则关闭时会发送
复位（RST）。

- 只在流上没有其他未完成操作时等待它。
- 达到 `timeout` 或 `discard_limit` 属于正常完成，不是错误。
- `timeout` 为零或负数、或 `discard_limit` 为零时，半关闭后立即关闭。
- 任何情况下套接字都会被关闭：成功、达到上限、取消和出错。错误和取消在关闭之后仍会传播。
- 这个上限只约束协作式的排空，不能保证对端收到数据；持续发送的对端仍可能看到突然关闭。

#### `close`

```cpp
void close() noexcept;
```

取消流上挂起的等待并关闭原生句柄，`is_open()` 变为 `false`。可重复调用。挂起的操作随后以
`std::errc::operation_canceled`（或 `bad_file_descriptor`）恢复，因此在它们结束之前 `stream` 对象本身必须保持存活。
析构函数会调用 `close()`。

#### 访问器

| 成员 | 结果 |
|:--|:--|
| `native_handle()` | 借用的原生套接字。不要关闭它，也不要改变它的阻塞模式 |
| `context()` | 所属的 `io_context` |
| `is_open()` | 流是否拥有句柄；不反映连接是否健康 |
| `local_endpoint()` | 从原生套接字查询的本地地址和端口 |
| `remote_endpoint()` | 已连接对端的地址和端口；要求流已连接 |

`local_endpoint()` 和 `remote_endpoint()` 抛出 `std::system_error`：流已关闭时为 `bad_file_descriptor`，上下文已停止
时为 `operation_canceled`，其余为原生错误（例如对端地址不可用）。

### `listener`

```cpp
class listener
{
public:
    listener(io_context& context, endpoint endpoint, listen_options options = {});
    ~listener();
    listener(listener&& other) noexcept;
    listener& operator=(listener&& other) noexcept;
    listener(const listener&) = delete;
    listener& operator=(const listener&) = delete;

    task<stream> accept();
    [[nodiscard]] endpoint local_endpoint() const;
    [[nodiscard]] io_context& context() const noexcept;
    [[nodiscard]] native_socket_type native_handle() const noexcept;
    void close() noexcept;
};
```

只能移动的监听套接字。构造函数同步地创建、绑定并开始监听，可以在上下文运行之前调用。校验错误抛出
`std::invalid_argument`，操作系统错误抛出 `std::system_error`；构造失败时会关闭已创建的句柄。端口 `0` 绑定一个空闲
端口，用 `local_endpoint()` 读回。`0.0.0.0` 绑定所有 IPv4 接口，`::` 绑定所有 IPv6 接口。

`accept()` 等待一个连接，返回一个属于监听器所在上下文的、已连接的新 `stream`。接受的套接字是非阻塞的。在被接受之前
就已中止或复位的连接会被跳过。进程耗尽描述符或套接字缓冲区时，`accept()` 退避 50 ms 后重试而不是失败。连续接受 64
个连接后会让出一次反应器，让其他工作得以执行。其他错误抛出 `std::system_error`。

`close()` 取消挂起的 `accept()`，后者随后抛出错误码为 `operation_canceled` 或 `bad_file_descriptor` 的
`std::system_error`。已接受的流不受影响。监听器没有 `is_open()`；需要时把 `native_handle()` 与
`pace::invalid_socket` 比较。`local_endpoint()`、`context()`、`native_handle()`、移动和析构遵循 `stream` 的规则。

## 函数

### `connect`

```cpp
task<stream> connect(endpoint destination, client_options options = {});
task<result<stream>> connect(as_result_t, endpoint destination, client_options options = {});
```

连接到数值端点并返回属于当前 context 的流。显式设置的 `options.context` 必须是执行此任务的 context。流使用期间该 context 必须存活。

### `listen`

```cpp
listener listen(io_context& io, endpoint local, listen_options options = {});
listener listen(endpoint local, listen_options options = {});
```

构造一个 `listener`：立即绑定并监听。不带上下文的重载在调用时就调用 `pace::this_context()`，因此必须在运行中上下文的
任务或回调里调用；要在 `io.run()` 之前创建监听器，请使用显式传入上下文的重载。

### `exchange` 与 `async_exchange`

```cpp
task<std::string> async_exchange(endpoint peer, std::string request, std::size_t reply_size, client_options options = {});
std::string exchange(endpoint peer, std::string request, std::size_t reply_size, client_options options = {});
```

一个连接、一个请求、一个恰好 `reply_size` 字节的应答。连接到 `peer`，写出
`request`，恰好读取 `reply_size` 字节，以默认的 100 ms / 64 KiB 上限优雅关闭，然后返回应答。不做连接池或复用。

- `reply_size` 是精确的分帧约定，而不是上限：收到较少字节后遇到 EOF 属于错误。允许为 `0`，此时请求仍会发出。
- `reply_size > options.max_reply_size.value_or(1024 * 1024)` 在连接之前抛出 `std::length_error`。
- 请求和应答字符串可以包含零字节。
- `async_exchange` 继承调用者的取消和期限；`options.timeout` 和 `options.deadline` 可缩短整个操作（含关闭）的预算。
- `exchange` 使用新建的 context，默认总超时为 30 秒。到期报告 `timed_out`。在反应器内阻塞调用会抛出 `std::logic_error`。

需要持久连接、其他分帧方式（分隔符、长度前缀）或可复用缓冲区时，请在自有的流上使用 `connect`、`pace::write` 和
`pace::read_exact`。

## 错误

| 情形 | 异常 |
|:--|:--|
| 端点格式错误、主机名、`%zone`、地址族或作用域不符 | `std::invalid_argument` |
| `listen_options::backlog <= 0`，或在 IPv4 端点上 `ipv6_only = false` | `std::invalid_argument` |
| 以显式地址族创建的流 `connect` 到另一地址族 | `std::system_error`（`address_family_not_supported`） |
| 同一个流第二次 `connect` | `std::logic_error` |
| 操作系统套接字错误（拒绝、复位、不可达等） | 带原生错误码的 `std::system_error` |
| `read_exact` / `exchange` 中途遇到 EOF | `std::system_error`（`connection_reset`） |
| `read_exact` / `exchange` 的长度超过 `maximum` | `std::length_error` |
| 在已关闭的流或监听器上操作 | `std::system_error`（`bad_file_descriptor`） |
| 上下文已停止、等待期间流被关闭，或任务被取消 | `std::system_error`（`operation_canceled`） |
| 等待方任务的截止时间或 `exchange` 的时间预算到期 | `std::system_error`（`timed_out`） |
| 在另一个正在运行的 `io_context` 上执行操作 | `std::logic_error` |
| 同一方向上第二个未完成操作 | `std::logic_error` |
| 不支持时使用 `reuse_port` | `std::system_error`（`operation_not_supported`） |

原生错误在 POSIX 上使用 `std::generic_category()`，在 Windows 上使用 `std::system_category()`；PACE 自己的情形使用
可移植的 `std::errc` 错误码。请与错误条件比较，例如 `error.code() == std::errc::operation_canceled`，不要比较数值
`errno`。在连接自己的任务里捕获每个连接的错误，避免一个出错的对端结束整个程序。

## 线程与并发

- 所有操作、`set_no_delay`、`close` 和访问器都在所属上下文的线程上调用，或者在上下文运行之前、停止之后调用。要从
  其他线程关闭流，请把调用 `post` 到它的上下文。PACE 会检查部分误用（在另一个正在运行的上下文上执行操作会抛出
  `std::logic_error`），但并非全部。
- 一个流同时允许一个未完成的读操作（`read_some`、`read_exact`）和一个未完成的写操作（`write_all`、`connect`），所以
  一个读任务和一个写任务可以共享同一个流。同一方向上已有操作挂起时再发起读或写，会在触碰套接字之前立即抛出
  `std::logic_error`，因此它既不会拿走第一个读操作的数据，也不会与第一个写操作的数据交错。
- 一个监听器同时只允许一个未完成的 `accept()`；第二个会立即抛出 `std::logic_error`。
- `close_gracefully()` 要求没有其他未完成操作。`shutdown_write()` 属于写方向：请在没有写操作未完成时调用（不做
  检查）；此时仍可以有一个读操作未完成。

## 生命周期与所有权

- `io_context` 的寿命必须长于在其上创建的所有流和监听器。
- 只要流或监听器还有挂起的操作，就要保持它存活且地址不变。关闭会让这些操作恢复，但在它们结束之前销毁对象属于未
  定义行为。
- 传给 `read_some`、`read_exact`（span 重载）和 `write_all` 的缓冲区在操作完成前一直被借用，`pace::as_bytes` 或
  `pace::as_writable_bytes` 视图背后的存储也是如此。`connect` 以及接受 `std::string` 的辅助函数拥有自己的参数。
- 只在没有未完成操作时移动流或监听器。移动后源对象不再持有句柄（但仍绑定原上下文）；移动赋值会先关闭目标原有的
  套接字。
- 接受得到的流属于监听器的上下文，无法重新绑定到其他上下文。

## 限制与默认值

| 项目 | 默认值或上限 |
|:--|:--|
| `endpoint::address` / `port` / `family` / `scope_id` | `"127.0.0.1"` / `0` / `unspecified` / `0` |
| `listen_options::backlog` | 1024（必须 > 0；操作系统可能截断） |
| `listen_options::reuse_port` / `ipv6_only` | `false` / `true` |
| `close_gracefully` 超时 / 丢弃上限 | 100 ms / 64 KiB |
| `read_exact` 的 `maximum` / `exchange` 的 `max_reply_size` | 1 MiB |
| `exchange` 超时（`client_options::timeout`） | 30 s |
| `write_all` 让出反应器之前发送的字节数 | 1 MiB |
| `accept()` 让出之前连续接受的连接数 | 64 |
| 描述符或缓冲区耗尽时 `accept()` 的退避时间 | 50 ms |
| 连接超时、读写超时 | 无；请使用截止时间 |

## 参见

- [网络概览](README.zh.md)、[UDP](udp.zh.md)
- TCP 之上的 [TLS](../tls/README.zh.md)，主机名解析用 [DNS](../dns/README.zh.md)
- [I/O 上下文](../runtime/io-context.zh.md)、[运行程序](../runtime/run.zh.md)、
  [结构化并发](../runtime/structured-concurrency.zh.md)
- 教程：[TCP](../../tutorial/05-tcp.zh.md)、[优雅关闭](../../tutorial/09-graceful-shutdown.zh.md)
- 指南：[协议调优](../../guides/protocol-tuning.zh.md)
