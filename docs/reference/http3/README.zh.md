<!-- source-hash: bdb2dc384315 -->
# HTTP/3

[English](README.md) · [参考](../README.zh.md)

## 概述

`pace::http3` 是 HTTP/3（RFC 9114）客户端和服务端，运行在协商了 ALPN `h3` 的已认证 [QUIC](../quic/README.zh.md)
连接之上，用 nghttp3 处理分帧和 QPACK。请求和响应使用 [`<pace/http.hpp>`](../http/README.zh.md) 中与协议无关的
`http::request` 和 `http::response`；消息体完整缓冲。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/http3.hpp>` | `pace_http3` | `--http3=y`（同时启用 `--quic=y`） |

本模块依赖 nghttp3 1.18.0 和 QUIC 模块（任一后端），定义 `PACE_HAS_HTTP3`，不使用 HTTP/1、HTTP/2 或 TLS 模块。与
QUIC 一样只支持 POSIX。启用 `--dns=y` 后 URL 客户端会解析主机名，否则 URL 中必须是数字地址。对应的 C++ 命名模块
是 `pace.http3`。

本模块有批量客户端（`async_exchange`、`async_get`），在你已建立的 QUIC 连接上发送请求；以及 URL 客户端（`fetch`、
`get`、`post` 及其 `async_` 形式），每次调用打开一条连接。

## 概要

```cpp
#include <pace/http3.hpp>
#include <pace/runtime.hpp>

#include <iostream>

pace::task<pace::http::response> hello(pace::http::request request)
{
    co_return pace::http::text("Hello over HTTP/3, " + request.target + "!");
}

int main()
{
    pace::quic::context server_config(pace::quic::role::server);
    server_config.use_certificate_chain("localhost.pem", "localhost-key.pem");
    server_config.set_alpn_protocols({"h3"});
    pace::quic::context client_config;
    client_config.load_verify_file("ca.pem");
    client_config.set_alpn_protocols({"h3"});

    pace::run(
        [&](pace::io_context& io) -> pace::task<void>
        {
            auto listener = pace::quic::listen(io, {"127.0.0.1", 0}, server_config);
            co_await pace::with_task_group(
                [&](pace::task_group& group) -> pace::task<void>
                {
                    auto server = group.spawn(pace::http3::serve(listener, hello));

                    auto connection =
                        co_await pace::quic::connect(client_config, listener.local_endpoint(), "localhost");
                    std::vector<pace::http::request> requests(2);
                    requests[0].method = "GET";
                    requests[0].target = "/one";
                    requests[1].method = "GET";
                    requests[1].target = "/two";
                    auto responses =
                        co_await pace::http3::async_exchange(std::move(connection), "localhost", std::move(requests));
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
    std::size_t max_concurrent_streams = 32;
    std::size_t max_requests_per_connection = 1000;
    std::size_t max_concurrent_handlers = 32;
};
```

每条连接的上限，客户端和服务端对收发的消息都适用。每个函数的最后一个参数都是 `limits`。

- `max_header_bytes`：每个流解码后的头部字节数（名称与值的长度之和，每个字段另加 32 字节）。初始头部和 trailers
  共用这一额度。
- `max_header_fields`：每个流的头部字段数。
- `max_body_bytes`：每个流每个方向缓冲的消息体。0 只允许空消息体。
- `max_concurrent_streams`：同时打开的请求流数，也是客户端批量请求的最大数量；对端允许的数量可能更少，此时客户端
  随额度到达再打开更多的流。
- `max_requests_per_connection`：一条连接整个生命周期内的请求数。
- `max_concurrent_handlers`：一条连接上同时运行的服务端 handler 任务数；实际上限为
  `min(max_concurrent_handlers, max_concurrent_streams)`。设为 1 时 handler 串行执行。

`max_header_bytes`、`max_header_fields` 和 `max_requests_per_connection` 必须大于零；`max_concurrent_streams` 和
`max_concurrent_handlers` 必须在 1–10,000 之间。

### HTTP/3 上的消息字段

共用消息类型到 HTTP/3 的映射与 [HTTP/2](../http2/README.zh.md#类型) 相同：收到的消息 version 为 `"HTTP/3"`，
`authority` 和 `target` 提供 `:authority` 和 `:path`，生成的帧信息取代 `Content-Length` 和 `Host`，HTTP/1 专用
字段被忽略，支持 trailers，HEAD 以及 204、205、304 不带消息体。不支持 `CONNECT`：客户端拒绝它，服务端回应 405。

## 函数

### 批量客户端

```cpp
task<std::vector<http::response>> async_exchange(quic::connection transport, std::string authority,
                                                 std::vector<http::request> requests, limits bounds = {});
```

在各自独立的双向流上发送整批请求，并按请求顺序返回响应。调用接管 `transport`（一条已建立且协商出 ALPN `h3` 的
连接），并在这批请求结束后关闭它。驱动协程同时处理读、写和流额度。一批必须包含 1 到 `max_concurrent_streams`
个请求，并且不超过 `max_requests_per_connection`。任何一个流被拒绝、重置、截断或格式错误，整个调用都会失败：
没有部分结果，也不会重试。

```cpp
task<http::response> async_get(quic::connection transport, std::string authority, std::string target = "/",
                               limits bounds = {});
```

只含一个 `GET target` 的批量请求。其他方法、自定义头部或多个请求请使用 `async_exchange`。

### 服务端

```cpp
task<void> serve_connection(quic::connection transport, http::handler handle, limits bounds = {});
```

服务一条已建立且协商出 `h3` 的连接，直到对端结束或连接失败。错误和取消会传播。

```cpp
task<void> serve(quic::listener& listener, http::handler handle, limits bounds = {});
```

在 `listener` 上接受连接并逐一服务，直到监听器被关闭或调用被取消；随后取消并等待所有已接受的连接结束，再正常
返回。监听器的服务端 context 需要证书和 ALPN `{"h3"}`。失败的握手或连接只结束它自己；引擎失败会传播。

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

// The caller's QUIC client configuration.
task<http::response> async_fetch(io_context&, const quic::context&, std::string url, http::request,
                                 limits bounds = {});
task<http::response> async_get(io_context& io, const quic::context& trust, std::string url, limits bounds = {});
http::response fetch(const quic::context& trust, std::string url, http::request request,
                     blocking_options wait = {}, limits bounds = {});
http::response get(const quic::context& trust, std::string url, blocking_options wait = {}, limits bounds = {});
```

每次调用打开一条 QUIC 连接，发送一个请求并返回其响应。只接受 `https://` URL。不提供 `quic::context` 时使用系统
信任库和 ALPN `h3`；调用方提供的 context 必须自行设置 ALPN `{"h3"}`，并比调用活得更久。连接只发往第一个解析出的
地址。这些调用与其他 URL 客户端共用 URL、请求和截止时间规则，见 [URL 客户端](../http/clients.zh.md)。阻塞形式在
私有 context 上运行，在 PACE context 内调用会抛出 `std::logic_error`。

## 错误

| 情况 | 异常 |
|:--|:--|
| 无效的 `limits`、连接未协商 ALPN `h3`、空 authority、批量为空或过大、无效请求或发出的头部、`CONNECT`、非 `https` URL | `std::invalid_argument` |
| 头部或消息体超出上限 | `std::length_error`；连接失败 |
| 对端协议错误、格式错误的消息、流被拒绝、重置或不完整、GOAWAY 拒绝了某个请求、对端打开过多的流 | `http::protocol_error` |
| 取消，或 context 已停止 | `std::system_error`，`std::errc::operation_canceled` |
| QUIC 失败 | 同 [QUIC](../quic/README.zh.md#错误) 所列 |

handler 抛出异常时，该流得到 `500 Internal Server Error` 响应。`serve()` 隔离各个已接受的连接；
`serve_connection()` 和 `async_exchange()` 会传播错误。

## 线程与并发

- 所有调用都在拥有 QUIC 连接的 context 上运行。每条连接由一个驱动协程独占 nghttp3 和 QUIC 流，它等待 QUIC 事件，
  从不使用固定时长的休眠。
- 在一条连接内，已完整接收的请求并发运行各自的 handler，最多 `min(max_concurrent_handlers, max_concurrent_streams)`
  个。handler 的结果进入有界队列并显式唤醒驱动协程。等待异步 I/O 的 handler 不会阻塞协议推进或其他流；handler 中
  的同步工作会阻塞 context。共享的 handler 状态必须能容忍协程交错执行。
- 响应可能乱序完成；批量结果保持请求顺序。

## 生命周期与所有权

- `async_exchange`、`async_get`、`serve_connection` 和 URL 客户端接管各自的连接，并在完成前关闭它。
- `serve()` 借用 `listener`，它必须比调用活得更久。关闭监听器会在 `serve()` 返回前取消并等待所有已接受的连接。
- 每条连接打开自己的控制流以及 QPACK 编码器流和解码器流。响应消息体保持存活，直到 QUIC 确认。
- 连接结束时，排队中的请求被丢弃，正在运行的 handler 被取消，并在调用返回前等待它们结束。handler 内的 PACE 等待
  会自动观察到这一取消；自定义 awaiter 必须参与[取消](../runtime/structured-concurrency.zh.md)。

## 限制与默认值

| 设置 | 默认值 | 范围 |
|:--|--:|:--|
| `max_header_bytes` | 每流 16 KiB，头部与 trailers 合计 | 非零 |
| `max_header_fields` | 每流 100 | 非零 |
| `max_body_bytes` | 每流每方向 1 MiB | 0 = 只允许空消息体 |
| `max_concurrent_streams` | 32 | 1–10,000 |
| `max_requests_per_connection` | 1,000 | 非零 |
| `max_concurrent_handlers` | 32 | 1–10,000 |
| QPACK 动态表 | 0（只用静态表和字面量） | |
| URL 客户端信任 | 系统信任库，ALPN `h3` | |

内存峰值随并发流数量与消息体大小的乘积增长。

不提供：流式消息体、CONNECT 隧道、服务器推送、HTTP Datagram、WebTransport 以及可复用的客户端连接。

## 参见

- 传输层 [QUIC](../quic/README.zh.md)
- [HTTP 消息与 handler](../http/README.zh.md)、[路由](../http/router.zh.md)、[URL 客户端](../http/clients.zh.md)
- [HTTP/2](../http2/README.zh.md)、[协议调优](../../guides/protocol-tuning.zh.md)
- 教程：[QUIC 与 HTTP/3](../../tutorial/13-quic-and-http3.zh.md)
- [可运行示例](../../../examples/tutorial_http3.cpp)
- [RFC 9114](https://www.rfc-editor.org/rfc/rfc9114)、[RFC 9204](https://www.rfc-editor.org/rfc/rfc9204)、
  [nghttp3 programmer's guide](https://nghttp2.org/nghttp3/programmers-guide.html)
