# 10. Multiplex requests with HTTP/2

[中文](10-http2.zh-CN.md) · [Previous: WebSocket](09-websocket.md) · [Next: QUIC/HTTP3](11-quic-http3.md)

HTTP/1 reused one connection sequentially. Here a batch submits independent
requests on different HTTP/2 streams. Start with cleartext prior knowledge so the
multiplexing step is visible before adding TLS. Enable `--http2=y`, link
`pace_http2`, and include `<pace/http2.hpp>` and `<pace/http_router.hpp>`.

## 1. Reuse the application handler

Use the explicit listener lifetime from chapter 4. Register routes, then change
the serve call; the shared request/response types stay the same:

```cpp
pace::http::router app;
app.get("/one", [](pace::http::request) { return pace::http::response{.body = "one"}; });
app.get("/two", [](pace::http::request) { return pace::http::response{.body = "two"}; });
pace::http2::limits limits;
limits.max_concurrent_handlers = 8;
io.spawn(pace::http2::serve(listener, app, limits));
```

The actual handler cap is also bounded by `max_concurrent_streams`. One protocol
driver owns nghttp2 and the transport; handlers may await independently. They do
not run CPU code in parallel on one reactor. Set the cap to one when sequential
application dispatch is required. Shared captures must outlive all active handlers,
including handlers finishing after a normal peer disconnect.

## 2. Submit a batch from the client coroutine

```cpp
pace::tcp::stream transport(io);
co_await transport.connect(endpoint);
std::vector<pace::http::request> requests(2);
requests[0].method = "GET"; requests[0].target = "/one";
requests[1].method = "GET"; requests[1].target = "/two";
const auto replies = co_await pace::http2::exchange(
    std::move(transport), "localhost", std::move(requests));
```

`exchange` takes ownership of the transport for this batch. Results preserve input
order even if the second handler finishes first. Inspect both status and body.
Bodies remain buffered; this is not a streaming response API. Do not mix HTTP/1
bytes or an Upgrade handshake into this prior-knowledge connection.

## 3. Observe overlap, then add TLS

Build/run `tutorial_http2` from the repository after enabling HTTP2. Its
[complete program](../../examples/tutorial_http2.cpp) combines the same listener
and batch lifecycle and prints the two responses. In your version, make `/one`
await a timer and `/two` return immediately. Log when each handler completes; the
handler log can be out of order while the returned vector remains ordered.

For HTTPS enable `--tls=y`, configure both TLS peers with ALPN `h2`, and use the
verified handshake from chapter 8. The server overload accepts the TLS
configuration; the client `exchange` receives an already handshaken TLS stream.
A wrong ALPN is an error, not a silent fallback to HTTP/1.

Stop only after your batch/control task finishes, keeping the listener and TLS
configuration outside those tasks. [HTTP/2 reference](../reference/http2.md)
describes flow-control bounds, half-close behavior and handler lifetime.
