<!-- source-hash: 4b47e40b3b39 -->
# HTTP/2

[English](README.md) · [参考](../README.zh.md)

## 概述

`pace::http2` 是基于 nghttp2 的 HTTP/2 客户端和服务端，运行在明文 TCP（prior knowledge）或协商了 ALPN `h2` 的
已验证 TLS 之上。请求和响应使用 [`<pace/http.hpp>`](../http/README.zh.md) 中与协议无关的 `http::request` 和
`http::response`；消息体完整缓冲。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/http2.hpp>` | `pace_http2` | `--http2=y`；TLS 形式还需要 `--tls=y` |

本模块依赖 nghttp2 1.70.0，定义 `PACE_HAS_HTTP2`，不使用 HTTP/1 模块。启用 `--dns=y` 后 URL 客户端会解析主机名，
否则 URL 中必须是数字地址。对应的 C++ 命名模块是 `pace.http2`。

本模块有两类客户端：批量客户端（`async_exchange`、`async_get`）在你已打开的连接上发送请求；URL 客户端
（`fetch`、`get`、`post` 及其 `async_` 形式）每次调用打开一条连接。

## 概要

```cpp
#include <pace/http2.hpp>
#include <pace/runtime.hpp>

#include <iostream>

pace::task<pace::http::response> hello(pace::http::request request)
{
    co_return pace::http::text("Hello, " + request.target + "!");
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
                    auto server = group.spawn(pace::http2::serve(listener, hello));

                    std::vector<pace::http::request> requests(2);
                    requests[0].method = "GET";
                    requests[0].target = "/one";
                    requests[1].method = "GET";
                    requests[1].target = "/two";
                    auto responses = co_await pace::http2::async_exchange(
                        co_await pace::tcp::connect(listener.local_endpoint()), "localhost", std::move(requests)
                    );
                    for (const auto& response : responses) // In request order.
                    {
                        std::cout << response.status << ' ' << response.body << '\n';
                    }

                    listener.close(); // serve() joins its connections and returns.
                    co_await server;
                }
            );
        }
    );
}
```

## 类型

### `limits`

```cpp
struct limits
{
    std::size_t max_header_bytes = std::size_t{16} * 1024;
    std::size_t max_header_fields = 100;
    std::size_t max_body_bytes = std::size_t{1024} * 1024;
    std::size_t max_concurrent_streams = 100;
    std::size_t max_requests_per_connection = 1000;
    std::size_t max_concurrent_handlers = 100;
};
```

每条连接的上限，客户端和服务端都使用。每个函数的最后一个参数都是 `limits`。

- `max_header_bytes`：每个流的头部字节数，按 HTTP/2 头部列表大小计算（名称与值的长度之和，每个字段另加 32 字节）。
  初始头部、信息性头部和 trailers 共用这一额度。
- `max_header_fields`：每个流的头部字段数。
- `max_body_bytes`：每个流每个方向缓冲的消息体。0 只允许空消息体，从不表示无限制。
- `max_concurrent_streams`：同时打开的流数，也是客户端批量请求的最大数量。
- `max_requests_per_connection`：一条连接整个生命周期内的请求数。
- `max_concurrent_handlers`：一条连接上同时运行的服务端 handler 任务数；实际上限为
  `min(max_concurrent_handlers, max_concurrent_streams)`。设为 1 时 handler 串行执行。

`max_header_bytes`、`max_header_fields` 和 `max_requests_per_connection` 必须大于零；`max_concurrent_streams` 和
`max_concurrent_handlers` 必须在 1–10,000 之间。对端违反上限会使连接失败。

### HTTP/2 上的消息字段

`http::request` 和 `http::response` 由 HTTP/1.1、HTTP/2 和 HTTP/3 共用。在 HTTP/2 上：

- 收到的消息 `version` 为 `"HTTP/2"`。头部名称以小写发送。
- `authority` 参数（或 URL）提供 `:authority`；请求的 `target` 提供 `:path`。target 以 `/` 开头；只有
  `OPTIONS` 可以使用 `*`。不支持 `CONNECT`：客户端拒绝它，服务端回应 405。
- `headers` 中的 `Content-Length` 和 `Host` 字段会被生成的帧信息和 authority 取代。请求的 `Content-Type` 头部
  会保留；响应的 Content-Type 取自 `response.content_type`。
- 连接级字段（`Connection`、`Keep-Alive`、`Proxy-Connection`、`Transfer-Encoding`、`Upgrade`）以及值不是
  `trailers` 的 `TE` 会被拒绝。
- `request.keep_alive`、`request.chunked`、`response.close`、`response.chunked` 以及发出消息的 `version` 都被忽略：
  HTTP/2 有自己的分帧和流生命周期。特别是 `response.close` 不会关闭连接。
- 支持发送和接收 trailers。HEAD 请求以及 204、205、304 响应不带消息体。

## 函数

### 批量客户端

```cpp
task<std::vector<http::response>> async_exchange(tcp::stream transport, std::string authority,
                                                 std::vector<http::request> requests, limits bounds = {});
task<std::vector<http::response>> async_exchange(tls::stream transport, std::string authority,
                                                 std::vector<http::request> requests, limits bounds = {});
```

一次发送整批请求，每个请求占用一个流，并按请求顺序返回响应，与各流完成的顺序无关。调用接管 `transport`，并在
这批请求结束后关闭它。`authority` 是 HTTP authority（主机及可选端口），与传输层所连接的地址无关。一批必须包含
1 到 `max_concurrent_streams` 个请求，并且不超过 `max_requests_per_connection`。任何一个流失败、被重置或不完整，
整个调用都会抛出异常：没有部分结果，也不会重试。

TCP 形式使用 HTTP/2 prior knowledge：直接以连接前言开始，不经过 HTTP/1.1 `Upgrade: h2c`。TLS 形式（`--tls=y`）
要求流已完成验证握手并协商出 ALPN `h2`：连接前用 `set_alpn_protocols({"h2"})` 配置客户端 `tls::context`。

```cpp
template <class Transport> // tcp::stream, or tls::stream with --tls=y
task<http::response> async_get(Transport transport, std::string authority, std::string target = "/",
                               limits bounds = {});
```

只含一个 `GET target` 的批量请求。返回该响应，之后关闭连接。其他方法、自定义头部或多个请求请使用
`async_exchange`。

### 服务端

```cpp
task<void> serve_connection(tcp::stream transport, http::handler handle, limits bounds = {});
task<void> serve_connection(tls::stream transport, http::handler handle, limits bounds = {});
```

服务一条连接，直到对端结束或连接失败。调用接管 `transport`。TLS 形式要求握手已完成并协商出 `h2`。错误和取消
传播给调用方。

```cpp
task<void> serve(tcp::listener& listener, http::handler handle, limits bounds = {});
task<void> serve(tcp::listener& listener, const tls::context& configuration, http::handler handle,
                 limits bounds = {});
```

接受连接并逐一服务，直到 `listener` 被关闭或调用被取消；随后取消并等待所有已接受的连接结束，再正常返回。失败的
连接（TLS 形式中还包括失败的握手）只结束它自己。TLS 形式用 `configuration` 完成每次握手，该配置必须带有证书和
ALPN `{"h2"}`。

`http::handler` 是 `std::function<task<http::response>(http::request)>`；用路由器构造 handler 见
[路由](../http/router.zh.md)。

### URL 客户端

```cpp
task<http::response> async_fetch(io_context&, std::string url, http::request, limits bounds = {});
task<http::response> async_get(io_context& io, std::string url, limits bounds = {});
task<http::response> async_post(io_context& io, std::string url, std::string body,
                                std::string content_type = "application/octet-stream", limits bounds = {});
task<http::response> async_fetch(std::string url, http::request request, limits bounds = {});
task<http::response> async_get(std::string url, limits bounds = {});
task<http::response> async_post(std::string url, std::string body,
                                std::string content_type = "application/octet-stream", limits bounds = {});
http::response fetch(std::string url, http::request request, blocking_options wait = {}, limits bounds = {});
http::response get(std::string url, blocking_options wait = {}, limits bounds = {});
http::response post(std::string url, std::string body, std::string content_type = "application/octet-stream",
                    blocking_options wait = {}, limits bounds = {});

// With --tls=y: the caller's TLS configuration, for https:// only.
task<http::response> async_fetch(io_context&, const tls::context&, std::string url, http::request,
                                 limits bounds = {});
task<http::response> async_get(io_context& io, const tls::context& trust, std::string url, limits bounds = {});
http::response fetch(const tls::context& trust, std::string url, http::request request,
                     blocking_options wait = {}, limits bounds = {});
http::response get(const tls::context& trust, std::string url, blocking_options wait = {}, limits bounds = {});
```

每次调用打开一条连接，发送一个请求并返回其响应。`http://` URL 使用 prior knowledge；`https://` URL 使用系统信任库
和 ALPN `h2` 的 TLS（未启用 `--tls=y` 时，`https://` URL 会抛出 `std::errc::not_supported` 的
`std::system_error`）。调用方提供的 `tls::context` 必须自行设置 ALPN `{"h2"}`，并比调用活得更久。这些调用与
HTTP/1、HTTP/3 的 URL 客户端共用 URL、请求和截止时间规则，见 [URL 客户端](../http/clients.zh.md)。阻塞形式在私有
context 上运行，在 PACE context 内调用会抛出 `std::logic_error`。

## 错误

| 情况 | 异常 |
|:--|:--|
| 无效的 `limits`、空 authority、批量为空或过大、无效请求或发出的头部、`CONNECT` | `std::invalid_argument` |
| 发出的头部超出上限 | `std::length_error` |
| TLS 传输未协商 ALPN `h2`、对端协议错误、流被重置或不完整、GOAWAY 拒绝了某个请求 | `std::runtime_error` |
| 收到的头部或消息体超出上限 | `std::length_error`；连接失败 |
| 服务端：请求在输入结束时被截断，或之后响应输出被阻塞 | `http::protocol_error` |
| 从其他 context 调用 | `std::logic_error` |
| 取消，或 context 已停止 | `std::system_error`，`std::errc::operation_canceled` |
| 套接字和 TLS 错误 | 传输层抛出的 `std::system_error` |

handler 抛出异常时，该流得到 `500 Internal Server Error` 响应，连接继续工作。handler 被取消不会转换成响应。
`serve()` 隔离各个已接受的连接，因此这些错误不会到达其调用方；`serve_connection()` 和 `async_exchange()` 会传播它们。

## 线程与并发

- 所有调用都在拥有传输层的 context 上运行；一条连接由一个协程驱动，它独占 nghttp2 会话和传输层。
- 在一条连接内，已完整接收的请求并发运行各自的 handler，最多
  `min(max_concurrent_handlers, max_concurrent_streams)` 个。挂起等待异步 I/O 的 handler 不会阻塞其他流或协议推进，
  handler 完成时会唤醒驱动协程，无需轮询。这是单个 context 上的协作式并发，不是并行的 CPU 计算：执行同步工作的
  handler 会阻塞 context。请求之间共享的 handler 状态必须能容忍协程交错执行。
- handler 槽位已满时，就绪的请求在流上限之内等待；不存在无界队列。响应可能乱序完成。
- TCP 输入正常结束后，已完整接收的请求仍会运行，其响应使用对端剩余的流量控制额度；被截断的请求使连接失败，需要
  更多额度的响应也会失败，而不是一直等待。

## 生命周期与所有权

- `async_exchange`、`async_get`、`serve_connection` 和 URL 客户端接管各自的传输层，并在完成前关闭它。
- `serve()` 借用 `listener`，TLS 形式还借用 `configuration`：两者都必须比调用活得更久。关闭监听器会在 `serve()`
  返回前取消并等待所有已接受的连接；停止 context 还会取消其上无关的工作。
- 连接结束时，排队中的请求被丢弃，正在运行的 handler 被取消，并在调用返回前等待它们结束。handler 内的 PACE 等待
  会自动观察到这一取消；自定义 awaiter 必须参与[取消](../runtime/structured-concurrency.zh.md)。handler 捕获的
  一切都必须保持有效直到那时。
- 被重置的流会丢弃其待发送的响应，但它的 handler 在结束前仍占用槽位。

## 限制与默认值

| 设置 | 默认值 | 范围 |
|:--|--:|:--|
| `max_header_bytes` | 每流 16 KiB，头部与 trailers 合计 | 非零 |
| `max_header_fields` | 每流 100 | 非零 |
| `max_body_bytes` | 每流每方向 1 MiB | 0 = 只允许空消息体 |
| `max_concurrent_streams` | 100 | 1–10,000 |
| `max_requests_per_connection` | 1,000 | 非零 |
| `max_concurrent_handlers` | 100 | 1–10,000 |
| URL 客户端 `https://` 信任 | 系统信任库，ALPN `h2` | |
| TLS 暂存缓冲区（基于 TLS 的 HTTP/2） | 每连接发送 64 KiB + 接收 64 KiB | |
| 每次 TLS 写入的明文 | 最多 16 KiB | |

内存峰值随活动流数量与消息体大小的乘积增长；请按应用的并发度选择上限。

不提供：HTTP/1.1 `Upgrade: h2c`、CONNECT 隧道（包括基于 HTTP/2 的 WebSocket）、服务器推送、流式消息体以及可复用的
客户端连接。

## 参见

- [HTTP 消息与 handler](../http/README.zh.md)、[路由](../http/router.zh.md)、[URL 客户端](../http/clients.zh.md)
- [HTTP/1](../http1/README.zh.md)、[HTTP/3](../http3/README.zh.md)、[TLS](../tls/README.zh.md)
- [协议调优](../../guides/protocol-tuning.zh.md)
- 教程：[HTTP/2](../../tutorial/12-http2.zh.md)
- [可运行示例](../../../examples/tutorial_http2.cpp)
- [RFC 9113](https://www.rfc-editor.org/rfc/rfc9113)
