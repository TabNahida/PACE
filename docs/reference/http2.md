# HTTP/2

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
auto responses = co_await pace::http2::exchange(
    std::move(stream), "localhost", std::move(requests));
```

All requests are submitted together. `responses[i]` belongs to `requests[i]`,
even when the peer completes streams in a different order. `exchange` owns and
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
Within one connection, completed requests invoke handlers sequentially; a slow
handler delays that connection. Bodies are buffered, not streamed to handlers.
An application exception produces HTTP 500. Cancellation propagates; malformed
or closed peers are isolated by the `serve` accept loop.

## HTTPS with ALPN

Enable both dependencies:

```sh
xmake f --tls=y --http2=y -y
xmake
```

Configure both TLS contexts with `set_alpn_protocols({"h2"})`. The client first
connects a `tls::stream` with a verified hostname, then passes it to `exchange`:

```cpp
pace::tls::context config;
config.set_alpn_protocols({"h2"});
pace::tls::stream stream(io, config);
co_await stream.connect({"127.0.0.1", 8443}, "localhost");
auto responses = co_await pace::http2::exchange(
    std::move(stream), "localhost:8443", std::move(requests));
```

For a private CA, call `config.load_verify_file(...)` before creating the stream.
For the server, load its certificate/key and call
`serve(listener, server_config, handler)`. This overload performs each handshake.
The direct TLS `serve_connection` and `exchange` overloads require an already
completed handshake whose selected protocol is `h2`; they reject other protocols.
See [TLS setup](tls.md).

## API and limits

Include `<pace/http2.hpp>`; APIs are available with `PACE_HAS_HTTP2` (provided by
the xmake `http2` option). Framing, HPACK and flow control use nghttp2.

| API | Result |
|---|---|
| `exchange(tcp::stream, authority, vector<http::request>, limits = {})` | `task<vector<http::response>>` |
| `exchange(tls::stream, authority, vector<http::request>, limits = {})` | Same, with TLS enabled |
| `serve_connection(tcp::stream, handler, limits = {})` | `task<void>`; owns one connection |
| `serve_connection(tls::stream, handler, limits = {})` | Same, after a completed TLS handshake |
| `serve(listener&, handler, limits = {})` | Accept cleartext connections until canceled |
| `serve(listener&, const tls::context&, handler, limits = {})` | Accept TLS connections until canceled |

`http::request` and `http::response` carry headers, buffered bodies and trailers.
Received versions are `HTTP/2`. Framing headers are generated automatically;
connection-specific headers are rejected. HEAD, 204, 205 and 304 suppress response
bodies. Header field names are sent in lowercase. `authority` supplies `:authority`;
use the endpoint's numeric address for the transport and the HTTP hostname here.

| `http2::limits` field | Default |
|---|---:|
| `max_header_bytes` | 16 KiB per stream, including trailers/informational headers |
| `max_header_fields` | 100 per stream |
| `max_body_bytes` | 1 MiB per stream, each direction |
| `max_concurrent_streams` | 100 |
| `max_requests_per_connection` | 1,000 |

Header bytes use the HTTP/2 header-list definition: name and value lengths plus
32 bytes per field. Initial headers and trailers share each stream's allowance.

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
每个请求使用独立 HTTP/2 流；结果按输入顺序返回。`exchange` 接管连接，并在
整批结束后关闭。如果任何流失败，整批抛出异常，不会自动重试。

启用 HTTPS 时使用 `--tls=y --http2=y`，客户端和服务端 TLS context 都设置
`set_alpn_protocols({"h2"})`。客户端完成证书和主机名验证后，将 `tls::stream`
传给 `exchange`。服务端的 TLS `serve` 重载会完成握手。

请求、响应和 trailers 使用与 HTTP/1.1 相同的数据结构。消息体完整缓冲后再交给
应用；同一连接内的 handler 按完成请求的顺序执行，慢 handler 会延迟该连接。
通过上表的 limits 限制消息、字段和并发流，避免随并发增加无限占用内存。
当前明文方式需要 HTTP/2 prior knowledge，不支持 HTTP/1.1 升级、CONNECT、
服务器推送或 HTTP/3。

[Runnable example](../../examples/tutorial_http2.cpp) · [Reference index](README.md)
