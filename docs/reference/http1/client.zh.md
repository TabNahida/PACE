<!-- source-hash: 1dca3cde158a -->
# HTTP/1 客户端

[English](client.md) · [HTTP/1](README.zh.md)

## 概述

发送 HTTP/1 请求有三种方式：持久连接的 `client`（TLS 上为 `secure_client`），在你打开的连接上依次发送请求；向数字端点发起的一次性调用；以及 URL 调用，可以在协程中使用（`async_get`、`async_post`、`async_fetch`），也可以作为直接阻塞调用（`get`、`post`、`fetch`）。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/http1.hpp>` | `pace_http1` | `--http1=y`（默认）；`secure_client`、`https://` 和 `tls::context` 重载需要 `--tls=y`；URL 中的主机名需要 `--dns=y` |

响应在[限制](README.zh.md#limits)范围内完整缓冲；4xx 或 5xx 状态是正常响应。任何调用都不跟随重定向、不重试、不走代理、不保存 Cookie、不解压。URL 调用遵循 [HTTP/1、HTTP/2 和 HTTP/3 共同的规则](../http/clients.zh.md)。

## 概要

```cpp
#include <pace/http1.hpp>

#include <iostream>

pace::task<void> two_requests(pace::io_context& io, pace::tcp::endpoint server)
{
    // A persistent connection: requests are sent one after another.
    pace::http::client client(co_await pace::tcp::connect(server, {.context = &io}));
    for (const char* target : {"/a", "/b"})
    {
        auto reply = co_await client.send({.method = "GET", .target = target, .headers = {{"Host", "localhost"}}});
        std::cout << target << ": " << reply.status << '\n';
    }

    // One-shot calls open and close their own connection.
    auto health = co_await pace::http::async_get(io, server, "/health");
    auto posted = co_await pace::http::async_post(io, "http://127.0.0.1:8080/items", R"({"id":1})", "application/json");
    std::cout << health.status << ' ' << posted.status << '\n';
}

int main()
{
    // A direct call blocks this thread and needs no running context.
    auto reply = pace::http::get("http://127.0.0.1:8080/health");
    std::cout << reply.body;
}
```

## 类型

### `basic_client`, `client`, `secure_client`

```cpp
template <class Transport>
class basic_client
{
public:
    explicit basic_client(Transport stream, limits bounds = {}, client_options options = {});
    basic_client(const basic_client&) = delete;
    basic_client& operator=(const basic_client&) = delete;
    ~basic_client() = default;
    task<response> send(request outgoing);

    [[nodiscard]] Transport& transport() noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    void close() noexcept;
};

using client = basic_client<tcp::stream>;
using secure_client = basic_client<tls::stream>;  // with PACE_HAS_TLS
```

一条持久的 HTTP/1 连接。只提供这两种实例化。客户端既不可复制也不可移动。

| 成员 | 作用 |
|:--|:--|
| 构造函数 | 接管一个已连接的 `tcp::stream`，或一个已完成握手的 `tls::stream`。`options` 无效时抛出 `std::invalid_argument` |
| `send(outgoing)` | 校验并写出 `outgoing`，然后读取一个最终响应。之后收到的字节保留给下一次交换 |
| `transport()` | 所拥有的流，供没有交换进行时做配置。不要直接读写它 |
| `is_open()` | 流是否打开、连接是否可以复用。不会探测对端 |
| `close()` | 立即关闭流；可重复调用。请在所属 context 上调用 |

`send` 按 [HTTP/1 页面](README.zh.md#http1-如何使用-request-与-response)所述写出请求。HTTP/1.1 请求必须自带 `Host`。对支持 ALPN 的 `tls::stream`，协商出的协议必须是 `http/1.1` 或没有。客户端先发完整个请求再读取；不做流水线。

收到以 Content-Length 或 chunked 分帧的响应后，连接仍可使用，除非任一方要求关闭（`request::keep_alive = false`，或响应的 `close == true`）。没有分帧的响应会一直读到服务器关闭连接，并结束这条连接。当连接在一个完整响应之后结束时，由 `client_options` 决定如何关闭。在响应完整之前发生任何失败，连接都会立即关闭。

### `client_close_policy`, `client_options`

```cpp
enum class client_close_policy
{
    no_wait,
    graceful
};

struct client_options
{
    client_close_policy close_policy = client_close_policy::no_wait;
    std::chrono::milliseconds close_timeout{100};
};
```

客户端在一条连接的最后一个响应之后如何关闭连接。

- `no_wait` 发送本端的 TCP FIN 或 TLS `close_notify`，不等待对端，然后释放流。这一步中的传输错误不会取代已经收到的响应；取消仍会传播。不使用 `close_timeout`。
- `graceful` 等待对端关闭，最多 `close_timeout`；为零时只发送一次关闭通知而不等待。关闭期间的错误和取消会传播。

两种策略都不会缩短以连接结束来界定的响应：它仍需要 TCP EOF，在 TLS 上则需要经过认证的 `close_notify`；没有它就结束的 TLS 连接属于截断错误。策略只作用于关闭阶段，从不作用于请求或响应读取，也不作用于保持连接的交换。

```cpp
pace::http::client client(
    std::move(connected_socket),
    {},
    {.close_policy = pace::http::client_close_policy::graceful, .close_timeout = std::chrono::milliseconds(25)}
);
```

## 函数

### 向端点发起的一次性调用

```cpp
task<response> async_fetch(
    io_context& context,
    tcp::endpoint endpoint,
    request outgoing,
    limits bounds = {},
    client_options options = {}
);
task<response> async_get(
    io_context& context,
    tcp::endpoint endpoint,
    std::string target = "/",
    limits bounds = {},
    client_options options = {}
);
// With PACE_HAS_TLS:
task<response> async_fetch(
    io_context& context,
    const tls::context& configuration,
    tcp::endpoint endpoint,
    std::string peer_name,
    request outgoing,
    limits bounds = {},
    client_options options = {}
);
task<response> async_get(
    io_context& context,
    const tls::context& configuration,
    tcp::endpoint endpoint,
    std::string peer_name,
    std::string target = "/",
    limits bounds = {},
    client_options options = {}
);
```

连接数字形式的 `endpoint`，以 `Connection: close` 发送一个请求，读取响应后关闭。不做 DNS 查询，也不解析 URL。`async_get` 发送 `GET target`；`async_fetch` 发送 `outgoing`，并强制把 `keep_alive` 设为 `false`。

请求没有 `Host` 时会自动添加：HTTP 使用端点地址，HTTPS 使用 `peer_name`；端口不是 80（HTTP）或 443（HTTPS）时附加端口，IPv6 地址加方括号（`[::1]:8080`）。你自己提供的 `Host` 会被保留。在 TLS 上，`peer_name` 是证书必须匹配的名字和 SNI 名；`endpoint` 只决定连接到哪里。`configuration` 选择信任和[验证模式](../tls/README.zh.md)；为它配置 ALPN `http/1.1`，或不配置 ALPN。

```cpp
pace::tls::context trust;
trust.load_verify_file("ca.pem");
auto reply = co_await pace::http::async_get(io, trust, {"127.0.0.1", 8443}, "localhost", "/health");
```

### 协程中的 URL 调用

```cpp
task<response> async_fetch(
    io_context& context,
    std::string url,
    request outgoing,
    limits bounds = {},
    client_options options = {}
);
task<response> async_get(io_context& context, std::string url, limits bounds = {}, client_options options = {});
task<response> async_post(
    io_context& context,
    std::string url,
    std::string body,
    std::string content_type = "application/octet-stream",
    limits bounds = {},
    client_options options = {}
);
task<response> async_fetch(std::string url, request outgoing, limits bounds = {}, client_options options = {});
task<response> async_get(std::string url, limits bounds = {}, client_options options = {});
task<response> async_post(
    std::string url,
    std::string body,
    std::string content_type = "application/octet-stream",
    limits bounds = {},
    client_options options = {}
);
// With PACE_HAS_TLS:
task<response> async_fetch(
    io_context& context,
    const tls::context& configuration,
    std::string url,
    request outgoing,
    limits bounds = {},
    client_options options = {}
);
task<response> async_get(
    io_context& context,
    const tls::context& configuration,
    std::string url,
    limits bounds = {},
    client_options options = {}
);
```

解析 `url`，主机是名字时进行解析，然后连接、发送一个请求并返回响应。一切都在 task 运行时发生，而不是在创建时。URL 决定目标和 `Host`；请求中不得包含 `Host` 字段。`async_post` 发送 `body`，`Content-Type` 为 `content_type`。不带 `io_context&` 的形式使用运行它们的 context。

`https://` URL 使用默认的 `tls::context`：依据系统根证书验证服务器，并提供 ALPN `http/1.1`。带 `configuration` 的重载改用你的配置；此时 URL 必须是 `https://`，并且 URL 中的主机仍然是证书名和 SNI 名，即使它是数字地址。解析出的地址按顺序尝试，直到某个 TCP 连接成功；此后不再重试任何东西。URL 语法和其余规则见 [URL 客户端页面](../http/clients.zh.md#url-规则)。

### 直接调用

```cpp
inline response fetch(
    std::string url,
    request outgoing,
    blocking_options wait = {},
    limits bounds = {},
    client_options options = {}
);
inline response get(std::string url, blocking_options wait = {}, limits bounds = {}, client_options options = {});
inline response post(
    std::string url,
    std::string body,
    std::string content_type = "application/octet-stream",
    blocking_options wait = {},
    limits bounds = {},
    client_options options = {}
);
// With PACE_HAS_TLS:
inline response fetch(
    const tls::context& configuration,
    std::string url,
    request outgoing,
    blocking_options wait = {},
    limits bounds = {},
    client_options options = {}
);
inline response get(
    const tls::context& configuration,
    std::string url,
    blocking_options wait = {},
    limits bounds = {},
    client_options options = {}
);
```

在调用线程上新建一个 `io_context`，运行对应的 URL 调用并返回其响应。`wait` 限制整个调用（DNS、连接、TLS、交换和关闭）；其 `timeout` 必须为正，`deadline` 和 `cancellation` 可以进一步缩短它（见 [`blocking_options`](../runtime/run.zh.md)）。没有带 TLS 配置的 `post`：请在 `request` 中填好方法、正文和 `Content-Type`，然后调用 `fetch`。

## 错误

| 情况 | 异常 |
|:--|:--|
| 响应格式错误、被截断或超限 | `http::protocol_error`；连接被关闭 |
| 请求无效（方法、目标、版本、字段名或值、受管字段、`Expect`、`Upgrade`、`CONNECT`、HTTP/1.1 缺少 `Host`、不带 `chunked` 的 trailer） | 在写出任何内容之前抛出 `std::invalid_argument`；持久客户端仍可使用 |
| 另一个 `send` 正在进行、连接已关闭或在其他 context 上调用 `send` | `std::logic_error` |
| `client_options` 无效（未知策略、`close_timeout` 为负） | 构造函数或一次性调用抛出 `std::invalid_argument` |
| 传输或 TLS 错误、取消 | `std::system_error`；连接被关闭 |
| TLS 对端协商出 `http/1.1` 以外的 ALPN 协议 | `std::system_error`，错误码 `std::errc::protocol_error` |
| URL 调用的错误（URL 错误、提供了 `Host`、缺少 TLS 或 DNS 支持） | 见 [URL 客户端页面](../http/clients.zh.md#错误) |
| 直接调用在运行中的 context 内进行、超时不为正、到达截止时间 | `std::logic_error`、`std::invalid_argument`、错误码为 `std::errc::timed_out` 的 `std::system_error` |

## 线程与并发

`basic_client` 属于它的流所在的 context：在该 context 上或在它运行之前构造，从该 context 上的 task 调用 `send`，并在那里调用 `close`。同一时刻只能有一次交换；并发的第二个 `send` 会抛出异常，不影响第一个。不同的客户端互相独立。一次性调用和 URL 调用运行在给定的（或正在运行它们的）context 上，可以同时运行任意多个。直接调用会阻塞调用线程，不得在 PACE context 的线程上运行。

## 生命周期与所有权

- `basic_client` 拥有它的流。在 `send` 完成之前，客户端、流以及流所属的 context 都必须保持存活且不被移动；销毁客户端会关闭连接。
- `send` 拥有它的请求。返回的 `response` 拥有自己的数据。
- 一次性调用和 URL 调用拥有自己的参数；`io_context&` 和 `tls::context&` 是借用的，必须比 task 活得更久。`tls::stream` 一旦构造就会保持其配置存活，但在那之前惰性 task 只是借用它。
- 直接调用拥有它用到的一切，返回或抛出异常后不会留下任何仍在运行的东西。

## 限制与默认值

| 项目 | 默认值 |
|:--|:--|
| `limits` | 见 [HTTP/1 页面](README.zh.md#限制与默认值) |
| `client_options::close_policy` | `no_wait` |
| `client_options::close_timeout` | 100 ms（仅 `graceful` 使用） |
| `async_get`（端点形式）的 `target` | `/` |
| `post` / `async_post` 的 `content_type` | `application/octet-stream` |
| 直接调用的超时（`blocking_options::timeout`） | 30 秒 |
| 协程调用的截止时间 | 无，只受调用方约束 |
| `https://` 的默认信任与 ALPN | 系统根证书，开启验证，ALPN `http/1.1` |
| 每个客户端同时进行的交换数 | 1 |

## 参见

- [HTTP/1](README.zh.md)、[HTTP/1 服务器](server.zh.md)
- [HTTP/1、HTTP/2 和 HTTP/3 的 URL 客户端](../http/clients.zh.md)、[HTTP 消息](../http/README.zh.md)
- [TCP](../net/tcp.zh.md)、[TLS](../tls/README.zh.md)、[DNS](../dns/README.zh.md)、[`pace::run` 与 `blocking_options`](../runtime/run.zh.md)
- 教程：[HTTP 客户端](../../tutorial/02-http-client.zh.md)、[TLS 与 DNS](../../tutorial/10-tls-and-dns.zh.md)；指南：[HTTP 实用做法](../../guides/http-recipes.zh.md)
- [RFC 9112，消息正文长度](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3)
