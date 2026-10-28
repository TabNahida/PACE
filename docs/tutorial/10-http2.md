# 10. Make your first HTTP/2 request

[中文](10-http2.zh-CN.md) · [Previous: WebSocket](09-websocket.md) · [Next: QUIC/HTTP3](11-quic-http3.md)

Enable `--http2=y` and link `pace_http2` in chapter 1's current-source project.
Create `h2_server` and `h2_client` targets as in chapter 3. Start with cleartext
HTTP/2 prior knowledge: no HTTP/1 Upgrade is involved.

## 1. Keep the route, change the protocol

Save as h2_server.cpp:

```cpp
#include <pace/http2.hpp>
#include <pace/http_router.hpp>
#include <pace/run.hpp>
#include <iostream>

pace::task<void> serve() {
    auto listener = pace::tcp::listen({"127.0.0.1", 9093});
    pace::http::router routes;
    routes.get("/health", [] { return pace::http::text("ok\n"); });
    co_await pace::http2::serve_connection(co_await listener.accept(), routes);
}
int main() {
    try { pace::run(serve); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

The router and response helpers are the same as HTTP/1. `serve_connection` owns
one accepted transport and runs HTTP/2 until the peer disconnects. This exercise
exits after that connection; `http2::serve(listener, routes)` accepts repeatedly.

## 2. Request a response directly

Save as h2_client.cpp:

```cpp
#include <pace/http2.hpp>
#include <pace/run.hpp>
#include <iostream>

pace::task<pace::http::response> get_health() {
    auto transport = co_await pace::tcp::connect({"127.0.0.1", 9093});
    co_return co_await pace::http2::get(std::move(transport), "localhost:9093", "/health");
}
int main() {
    try {
        auto reply = pace::run(get_health);
        std::cout << reply.status << " " << reply.body;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`http2::get` owns the connected transport, submits one GET and returns its response.
Inspect both status and body. Its authority is independent of the numeric address;
the helper takes a path, not a complete URL, and does not resolve DNS.

## 3. Verify, then add more requests

Build both targets. Start `xmake run h2_server`, then `xmake run h2_client`; expect
`200 ok`. Restart the server before the next client. Try another route and inspect
its 404 response. The repository's `xmake run tutorial_http2` checks GET and a
multi-request batch locally.

For several concurrent requests on one connection, move to
[`exchange` and handler limits](../reference/protocol-tuning.md#http2-and-http3).
A loop of single GETs creates separate batches and misses that multiplexing benefit.
For verified TLS, use chapter 8's helpers with ALPN `h2` on both sides; pass the
handshaken TLS stream to the same `get` helper. See the
[HTTP/2 reference](../reference/http2.md) for TLS server overloads and flow control.
