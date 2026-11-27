<!-- source-hash: 5919422c5c1d -->
# HTTP URL 客户端

[English](clients.md) · [HTTP](README.zh.md)

## 概述

接收绝对 `http://` 或 `https://` URL、返回完整响应的一次性 HTTP 调用。HTTP/1、HTTP/2 和 HTTP/3 各自提供同样的六个名字，参数顺序和所有权规则相同；协议由命名空间决定。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/http1.hpp>`（命名空间 `pace::http`，别名 `pace::http1`） | `pace_http1` | `--http1=y`（默认） |
| `<pace/http2.hpp>`（命名空间 `pace::http2`） | `pace_http2` | `--http2=y` |
| `<pace/http3.hpp>`（命名空间 `pace::http3`） | `pace_http3` | `--http3=y` |

HTTP/1 和 HTTP/2 使用 `https://` 需要 `--tls=y`（HTTP/3 总是使用 QUIC 自带的 TLS），URL 中使用主机名需要 `--dns=y`。使用数字 IPv4 或 IPv6 地址的 URL 两者都不需要。

| 操作 | 直接调用，阻塞当前线程 | 协程调用，在运行中的 context 上 |
|:--|:--|:--|
| GET | `get(url)` | `co_await async_get(url)` |
| POST | `post(url, body, content_type)` | `co_await async_post(url, body, content_type)` |
| 任意请求 | `fetch(url, request)` | `co_await async_fetch(url, request)` |

每次调用都打开自己的连接，发送一个请求，读取一个响应，然后关闭连接。不跟随重定向，不重试，不走代理，不保存 Cookie，不解压，调用之间也不复用连接。要在一条连接上发多个请求，请使用 HTTP/1 的 [`client`](../http1/client.zh.md)，或在 [HTTP/2](../http2/README.zh.md)、[HTTP/3](../http3/README.zh.md) 连接上使用 `async_exchange`。

## 概要

```cpp
#include <pace/http1.hpp>

#include <iostream>

pace::task<void> report(std::string url)
{
    auto reply = co_await pace::http::async_get(std::move(url));
    std::cout << reply.status << ' ' << reply.body.size() << " bytes\n";
}

int main()
{
    // Direct call: its own context and connection, 30-second total deadline by default.
    auto reply = pace::http::get("http://127.0.0.1:8080/health");
    std::cout << reply.status << '\n';

    // Coroutine call: runs on the current context and inherits its cancellation and deadline.
    pace::run([] { return report("http://127.0.0.1:8080/health"); });
}
```

## 函数

下面的签名是 HTTP/1 的。`limits` 是各协议自己的限制类型（`http::limits`、`http2::limits`、`http3::limits`）；`client_options` 只有 HTTP/1 才有。

### 直接调用

```cpp
response get(std::string url, blocking_options wait = {}, limits bounds = {}, client_options options = {});
response post(
    std::string url,
    std::string body,
    std::string content_type = "application/octet-stream",
    blocking_options wait = {},
    limits bounds = {},
    client_options options = {}
);
response fetch(
    std::string url,
    request outgoing,
    blocking_options wait = {},
    limits bounds = {},
    client_options options = {}
);
```

每个函数在调用线程上新建一个 `io_context`，在其上运行对应的协程调用，并返回拥有数据的响应。`wait` 是整个调用的总预算，见 [`blocking_options`](../runtime/run.zh.md)。不涉及辅助线程，也没有共享的全局事件循环。不得在 PACE 任务或回调内部进行直接调用。

### 协程调用

```cpp
task<response> async_get(io_context& context, std::string url, limits bounds = {}, client_options options = {});
task<response> async_post(
    io_context& context,
    std::string url,
    std::string body,
    std::string content_type = "application/octet-stream",
    limits bounds = {},
    client_options options = {}
);
task<response> async_fetch(
    io_context& context,
    std::string url,
    request outgoing,
    limits bounds = {},
    client_options options = {}
);

task<response> async_get(std::string url, limits bounds = {}, client_options options = {});
task<response> async_post(
    std::string url,
    std::string body,
    std::string content_type = "application/octet-stream",
    limits bounds = {},
    client_options options = {}
);
task<response> async_fetch(std::string url, request outgoing, limits bounds = {}, client_options options = {});
```

不带 `io_context&` 的形式在 task 开始运行时取用运行它的 context。返回的 task 是惰性的：在被 `co_await` 或 spawn 之前什么都不做，连 URL 解析也不做。它拥有自己的全部参数。

`async_get` 发送 `GET`。`async_post` 发送 `POST`，正文为 `body`，`Content-Type` 字段为 `content_type`。`async_fetch` 原样发送 `outgoing`（方法、头部、正文、trailer），只有目标和 authority 由 URL 决定。

### 使用显式信任配置的调用

```cpp
// HTTP/1 and HTTP/2, with --tls=y: Trust is tls::context. HTTP/3: Trust is quic::context.
response get(const Trust& trust, std::string url, blocking_options wait = {}, limits bounds = {});
response fetch(const Trust& trust, std::string url, request outgoing, blocking_options wait = {}, limits bounds = {});
task<response> async_get(io_context& context, const Trust& trust, std::string url, limits bounds = {});
task<response> async_fetch(
    io_context& context,
    const Trust& trust,
    std::string url,
    request outgoing,
    limits bounds = {}
);
```

HTTP/1 在这里同样在 `bounds` 之后多一个 `client_options options = {}`。URL 必须是 `https://`。该配置取代默认配置：它决定受信任的根证书或另一种[验证模式](../tls/README.zh.md)，其 ALPN 列表必须提供对应协议（`http/1.1`、`h2`、`h3`）。配置是借用而非复制的，必须存活到调用完成。没有带信任参数的 `post`，也没有不带 context 的 `async_` 形式；要发 POST，请填好 `request`（方法、正文、`Content-Type` 头部）后调用 `fetch`。

```cpp
pace::tls::context trust;
trust.load_verify_file("ca.pem");
trust.set_alpn_protocols({"http/1.1"});
auto reply = pace::http::get(trust, "https://localhost:8443/health");
```

### URL 规则

三个协议用同一种方式解析 URL。这是 HTTP 客户端使用的子集，不是通用的 URI 解析器。

- scheme 为 `http` 或 `https`，大小写不限，后接 `://`。
- 主机为数字 IPv4 地址、带方括号的 IPv6 地址（`[::1]`）或 ASCII DNS 名（每个标签 1–63 个字母、数字和 `-`，不以 `-` 开头或结尾；总长不超过 253 个字符）。
- 可选端口为 1 到 65535 的十进制数。`http` 默认 80，`https` 默认 443。
- 路径和查询串按原样发送；空路径变为 `/`，只有查询串没有路径时变为 `/?…`。片段（fragment）被丢弃。
- 会被拒绝的有：userinfo（`user@`）、空格、控制字符、反斜杠、非 ASCII 字符、百分号编码的主机、IPv6 zone 标识以及不带方括号的 IPv6 地址。非 ASCII 的路径和查询字节需自行编码；已有的百分号转义保持不变。

URL 中按原样书写的 authority 成为 `Host` 字段（HTTP/2 和 HTTP/3 上为 `:authority`）。其中的主机部分同时也是服务器证书必须匹配的名字和 SNI 名。请求中不得包含 `Host` 字段，在 HTTP/2 和 HTTP/3 上也不得包含任何以 `:` 开头的字段。

### 各协议的差异

| | HTTP/1 | HTTP/2 | HTTP/3 |
|:--|:--|:--|:--|
| `http://` | 基于 TCP 的 HTTP/1.1 | 基于 TCP、prior knowledge 方式的 HTTP/2 | 拒绝 |
| `https://` | TLS，ALPN `http/1.1`（或无） | TLS，ALPN `h2` | QUIC，ALPN `h3` |
| 信任类型 | `tls::context` | `tls::context` | `quic::context` |
| 解析出的地址 | 按顺序尝试，直到某个 TCP 连接成功 | 按顺序尝试，直到某个 TCP 连接成功 | 只用第一个 |
| 额外参数 | `client_options`（关闭策略） | 无 | 无 |
| 格式错误的响应 | `http::protocol_error` | 见 [HTTP/2](../http2/README.zh.md) | 见 [HTTP/3](../http3/README.zh.md) |
| 平台 | 全部 | 全部 | POSIX |

HTTP/1 调用发送 `Connection: close`，保留请求的 `version`（空表示 `HTTP/1.1`），响应一完整就结束，不等待服务器关闭连接，见 [`client_options`](../http1/client.zh.md)。没有长度信息的 HTTP/1 响应会一直读到服务器关闭连接。

## 错误

| 情况 | 异常 |
|:--|:--|
| URL 格式错误、请求中带 `Host`（或伪头部）、带信任参数却用 `http://`、在 HTTP/3 上用 `http://` | `std::invalid_argument` |
| 没有 TLS 支持却用 `https://`，没有 DNS 支持却用主机名 | `std::system_error`，错误码 `std::errc::not_supported` |
| DNS 查询失败或没有返回地址 | 解析器的 `std::system_error`，或 `std::errc::host_unreachable` |
| 连接、TLS 或 QUIC 失败 | `std::system_error`（尝试了多个地址时为最后一个地址的错误） |
| 响应格式错误、被截断或过大 | 各协议的错误（HTTP/1 为 `http::protocol_error`） |
| 取消 | `std::system_error`，错误码 `std::errc::operation_canceled` |
| 直接调用：到达截止时间 | 与 `std::errc::timed_out` 比较相等的 `std::system_error` |
| 直接调用：在运行中的 PACE context 内调用 | `std::logic_error`，在任何 I/O 之前抛出 |
| 直接调用：`wait.timeout` 为零或负数 | `std::invalid_argument` |
| HTTP/1：`client_options::close_timeout` 为负数 | `std::invalid_argument` |

4xx 或 5xx 状态是正常响应，不是异常。不会重试任何东西：连接建立之后发生的失败都会报告出来，即使请求可能已经到达服务器。

## 线程与并发

协程调用完全在一个 context 上运行，继承 `co_await` 它的 task 的取消令牌和截止时间。它自己没有截止时间；可以用 `with_deadline` 或 `with_options` 加以限制（见[结构化并发](../runtime/structured-concurrency.zh.md)）。任意数量的调用可以在同一个或不同的 context 上并发运行，各自拥有自己的连接。

直接调用会阻塞调用线程，直到拿到响应或到达截止时间。它不能在 PACE context 内运行，否则会阻塞该 context 的事件循环。其他事件循环（GUI、其他库）检测不到，只会被直接阻塞。它的 `wait.cancellation` 令牌可以从其他线程取消。

## 生命周期与所有权

协程调用拥有自己的 URL、正文、内容类型和请求。`io_context&` 和信任配置是借用的，必须比 task 活得更久。返回的 `response` 拥有自己的数据。直接调用在执行期间拥有它用到的一切，返回或抛出异常后不会留下任何仍在运行的东西。

## 限制与默认值

| 项目 | 默认值 |
|:--|:--|
| `blocking_options::timeout`（直接调用） | 总计 30 秒，涵盖 DNS、连接、TLS、交换和关闭 |
| 协程调用的截止时间 | 无，只受调用方约束 |
| `post` 的 `content_type` | `application/octet-stream` |
| 默认信任 | 系统根证书，验证证书和主机名 |
| 默认 ALPN | `http/1.1`、`h2` 或 `h3` |
| 响应大小 | 各协议的 `limits`（默认 `max_body_bytes` 1 MiB、`max_header_bytes` 16 KiB） |
| DNS 截止时间与重试 | 解析器的默认值 |
| TCP 连接超时 | 操作系统的默认值 |

## 参见

- [HTTP 消息](README.zh.md)
- [HTTP/1 客户端](../http1/client.zh.md)、[HTTP/2](../http2/README.zh.md)、[HTTP/3](../http3/README.zh.md)
- [`pace::run` 与 `blocking_options`](../runtime/run.zh.md)、[结构化并发](../runtime/structured-concurrency.zh.md)
- [TLS](../tls/README.zh.md)、[DNS](../dns/README.zh.md)
- 教程：[HTTP 客户端](../../tutorial/02-http-client.zh.md)；指南：[HTTP 实用做法](../../guides/http-recipes.zh.md)
