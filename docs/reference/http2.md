# HTTP/2

URL get/post/fetch and async forms: [unified client contract](http-clients.md).

Short forms and owning helpers: [component conveniences](component-conveniences.md).

Run two requests over one connection:

```sh
xmake f --http2=y -y
xmake
xmake run tutorial_http2
```

The [complete example](../../examples/tutorial_http2.cpp) starts both peers locally.
Its client uses a connected TCP stream:

```cpp
pace::tcp::stream stream(io);
co_await stream.connect({"127.0.0.1", 8080});
std::vector<pace::http::request> requests(2);
requests[0].method = "GET"; requests[0].target = "/one";
requests[1].method = "GET"; requests[1].target = "/two";
auto responses = co_await pace::http2::async_exchange(
    std::move(stream), "localhost", std::move(requests));
```

All requests are submitted together. `responses[i]` belongs to `requests[i]`,
even when the peer completes streams in a different order. `async_exchange` owns and
closes the connection after this batch. It throws if any response is incomplete
or reset; it does not return a partial batch or retry requests.

## Serve requests

```cpp
pace::task<pace::http::response> hello(pace::http::request request) {
    co_return pace::http::response{.body = "Hello, " + request.target};
}
// listener must outlive serve() and all accepted connections.
io.spawn(pace::http2::serve(listener, hello));
```

The server accepts independent connections and receives multiplexed streams.
Within one connection, completed requests invoke handlers concurrently, up to
`min(max_concurrent_handlers, max_concurrent_streams)`. A handler suspended on
asynchronous I/O does not block other streams or protocol progress. One driver
owns nghttp2 and the TCP/TLS transport; handler completions wake it without
polling. This is cooperative concurrency on the owning context, not CPU work on
additional threads. A handler doing synchronous work still blocks that context.
Bodies are buffered, not streamed to handlers.

Ready requests remain within the connection's stream limit while handler slots
are occupied; there is no unbounded application work queue. Set
`max_concurrent_handlers = 1` to serialize application handlers while keeping the
protocol driver responsive. Shared handler state must support coroutine
interleaving. Responses may finish out of order; the batch client still returns
results in request order. Reset streams discard their pending responses, but an
already-running handler occupies its slot until it finishes.

A clean TCP receive EOF preserves already-complete requests: handlers can finish
and responses can use the peer's remaining flow-control credit. No further
requests or WINDOW_UPDATE frames can arrive after EOF. Truncated requests fail
the connection; a response that needs additional credit also fails instead of
waiting indefinitely. Handler-completion waits do not poll the EOF socket.

Aborting a connection discards queued requests, requests local cancellation of
active handlers, and joins them before returning. Handler PACE waits inherit this
token automatically. Captures must remain valid through serve_connection; custom
awaiters must participate in cancellation. See [structured scopes](structured-concurrency.md).
An application exception produces HTTP 500. Direct `serve_connection` and
`async_exchange` calls propagate connection cancellation and errors; `serve` isolates
accepted-connection failures and returns normally when its listener is closed or
canceled. Closing the listener cancels and joins its accepted connections before `serve`
returns; stopping the context also cancels unrelated work on that context.

## HTTPS with ALPN

Enable both dependencies:

```sh
xmake f --tls=y --http2=y -y
xmake
```

Configure both TLS contexts with `set_alpn_protocols({"h2"})`. The client first
connects a `tls::stream` with a verified hostname, then passes it to `async_exchange`:

```cpp
pace::tls::context config;
config.set_alpn_protocols({"h2"});
pace::tls::stream stream(io, config);
co_await stream.connect({"127.0.0.1", 8443}, "localhost");
auto responses = co_await pace::http2::async_exchange(
    std::move(stream), "localhost:8443", std::move(requests));
```

For a private CA, call `config.load_verify_file(...)` before creating the stream.
For the server, load its certificate/key and call
`serve(listener, server_config, handler)`. This overload performs each handshake.
The direct TLS `serve_connection` and `async_exchange` overloads require an already
completed handshake whose selected protocol is `h2`; they reject other protocols.
See [TLS setup](tls.md).

The TLS driver stages ciphertext when the socket is backpressured, so two peers
can keep reading while their outbound records wait for space. Each application
write supplies at most 16 KiB of plaintext; the driver waits for staged output to drain
before accepting another. This internal path reserves 64 KiB for output and
64 KiB for input per HTTP/2 TLS connection (128 KiB total, retained until the TLS
stream is destroyed). Ordinary HTTP/1 and WebSocket TLS streams do not allocate
these buffers. Readiness wakes the driver without polling. Genuine OpenSSL
WANT retries retain their original buffer, and normal completion drains queued
ciphertext before the public TLS shutdown; errors discard it by closing TCP.

## API and limits

Include `<pace/http2.hpp>`; APIs are available with `PACE_HAS_HTTP2` (provided by
the xmake `http2` option). Framing, HPACK and flow control use nghttp2.

| API | Result |
|---|---|
| `async_exchange(tcp::stream, authority, vector<http::request>, limits = {})` | `task<vector<http::response>>` |
| `async_exchange(tls::stream, authority, vector<http::request>, limits = {})` | Same, with TLS enabled |
| `serve_connection(tcp::stream, handler, limits = {})` | `task<void>`; owns one connection |
| `serve_connection(tls::stream, handler, limits = {})` | Same, after a completed TLS handshake |
| `serve(listener&, handler, limits = {})` | Accept cleartext connections until canceled |
| `serve(listener&, const tls::context&, handler, limits = {})` | Accept TLS connections until canceled |

`http::request` and `http::response` carry headers, buffered bodies and trailers.
Received versions are `HTTP/2`. Framing headers are generated automatically;
connection-specific headers are rejected. HEAD, 204, 205 and 304 suppress response
bodies. Header field names are sent in lowercase. `authority` supplies `:authority`;
use the endpoint's numeric address for the transport and the HTTP hostname here.
Request targets start with `/`; only `OPTIONS` may use `*`.

The shared message types retain some HTTP/1-only fields. Outgoing `version`,
`request.keep_alive`, `request.chunked`, `response.close` and `response.chunked`
are ignored by this API; HTTP/2 uses its own framing and stream lifecycle.
In particular, setting `response.close` does not close an HTTP/2 connection.
Custom Content-Length and Host fields are replaced by generated framing and
`authority`; set response Content-Type through `response.content_type`.

| `http2::limits` field | Default |
|---|---:|
| `max_header_bytes` | 16 KiB per stream, including trailers/informational headers |
| `max_header_fields` | 100 per stream |
| `max_body_bytes` | 1 MiB per stream, each direction |
| `max_concurrent_streams` | 100 |
| `max_requests_per_connection` | 1,000 |
| `max_concurrent_handlers` | 100 |

Header bytes use the HTTP/2 header-list definition: name and value lengths plus
32 bytes per field. Initial headers and trailers share each stream's allowance.
Header byte/field limits and the connection request limit must be nonzero.
`max_concurrent_streams` and `max_concurrent_handlers` must be between 1 and 10,000. `max_body_bytes = 0`
permits empty bodies only; zero never means unlimited.

A batch must contain 1–`max_concurrent_streams` requests and fit the connection
request limit. A bound violation fails the connection. Peak buffered memory grows
with active streams and their body sizes: choose limits for your application's
concurrency. Network operations inherit the context's cancellation behavior.

Cleartext is HTTP/2 **prior knowledge**: connect directly with the HTTP/2 preface.
HTTP/1.1 `Upgrade: h2c`, CONNECT tunnels, server push, HTTP/3 and a persistent
incremental client API are not provided. Use [HTTP/1.1](http.md) or
[WebSocket](websocket.md) for those separate message APIs.

## 中文：从批量请求开始

先执行本页最前面的三条命令，`tutorial_http2` 会在同一条本地连接上发送两个请求。
每个请求使用独立 HTTP/2 流；结果按输入顺序返回。`async_exchange` 接管连接，并在
整批结束后关闭。如果任何流失败，整批抛出异常，不会自动重试。

启用 HTTPS 时使用 `--tls=y --http2=y`，客户端和服务端 TLS context 都设置
`set_alpn_protocols({"h2"})`。客户端完成证书和主机名验证后，将 `tls::stream`
传给 `async_exchange`。服务端的 TLS `serve` 重载会完成握手。

请求、响应和 trailers 使用与 HTTP/1.1 相同的数据结构。消息体完整缓冲后再交给
应用；同一连接内最多同时运行 `min(max_concurrent_handlers, max_concurrent_streams)`
个 handler。handler 等待异步 I/O 时，连接继续处理其他流；单个驱动协程独占协议
状态和传输，完成事件会显式唤醒它，没有固定间隔轮询。这是单 context 上的协作式
并发，不会把 CPU 计算自动放到多个线程。设置 `max_concurrent_handlers = 1` 可串行
执行应用 handler；等待的请求仍受并发流上限约束。共享的 handler 状态需要允许协程
交错执行。连接退出前会取消并等待已启动的 handler；PACE 等待继承局部取消令牌，
捕获对象需存活到 serve_connection 完成。自定义 awaiter 需要参与协作式取消。
通过上表的 limits 限制消息、字段和并发流，避免随并发增加无限占用内存。
请求头字节数、字段数和每连接请求数上限必须大于零，并发流与 handler 上限均为 1–10,000；
body 上限为零时只允许空 body。HTTP/1 专用的 keep_alive、chunked、response.close
等字段在此接口中不控制连接或分块；设置 response.close 不会关闭 HTTP/2 连接。
关闭 listener 会取消并等待其已有连接，再结束 serve；停止 context 还会取消其他工作。
当前明文方式需要 HTTP/2 prior knowledge，不支持 HTTP/1.1 升级、CONNECT、
服务器推送或 HTTP/3。

[Runnable example](../../examples/tutorial_http2.cpp) · [Reference index](README.md)
