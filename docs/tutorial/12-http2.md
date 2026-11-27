# 12. HTTP/2

[中文](12-http2.zh.md) · [Learning path](README.md) · [Previous: WebSocket](11-websocket.md)

HTTP/2 is optional: configure the `hello` project with `--http2=y` (xmake downloads nghttp2 the first time) and link
`pace_http2`. Set up `h2_server` and `h2_client` targets the same way as the TCP pair in [chapter 5](05-tcp.md). This
chapter starts with cleartext HTTP/2 with prior knowledge: there is no HTTP/1 upgrade step.

## Keep the route, change the protocol

Save this as `h2_server.cpp`:

```cpp
#include <pace/http.hpp>
#include <pace/http2.hpp>
#include <pace/runtime.hpp>
#include <iostream>

pace::task<void> serve()
{
    auto listener = pace::tcp::listen({"127.0.0.1", 9093});
    pace::http::router routes;
    routes.get("/health", [] { return pace::http::text("ok\n"); });
    co_await pace::http2::serve_connection(co_await listener.accept(), routes);
}

int main()
{
    try
    {
        pace::run(serve);
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

The router and the response helpers are the ones `http::app` uses for HTTP/1 in [chapter 3](03-http-server.md).
`serve_connection` takes one accepted connection and serves HTTP/2 on it until the peer disconnects. This exercise
exits after that connection; `http2::serve(listener, routes)` accepts connections in a loop.

## Request a response directly

Save this as `h2_client.cpp`:

```cpp
#include <pace/http2.hpp>
#include <iostream>

int main()
{
    try
    {
        auto reply = pace::http2::get("http://127.0.0.1:9093/health");
        std::cout << reply.status << " " << reply.body;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`http2::get(url)` returns the response directly, with the same 30-second default budget as the HTTP/1 call. In a
coroutine, write `co_await pace::http2::async_get(url)`. Hostnames need `--dns=y`; `https://` URLs need `--tls=y`.
See [URL clients](../reference/http/clients.md).

## Verify, then add more requests

Build both targets. Start `xmake run h2_server`, then run `xmake run h2_client`; expect `200 ok`. Start the server
again before the next client. Request another path and look at the 404 response.

The companion [`tutorial_http2.cpp`](../../examples/tutorial_http2.cpp) serves a coroutine handler with
`http2::serve`, sends one GET over a connection it opened itself with `http2::async_get(stream, authority, target)`,
then sends `/one` and `/two` as one batch on a second connection with `http2::async_exchange`. `xmake run
tutorial_http2` (configured with `--http2=y --examples=y`) prints `Hello, /one!` and `Hello, /two!`.

`async_exchange` is how several requests share one connection: the batch is sent together and the responses come
back in request order. A loop of single GETs opens a connection for each and loses that multiplexing. For verified
TLS, set ALPN `h2` on both sides with the contexts from [chapter 10](10-tls-and-dns.md), and pass the handshaken TLS
stream to `async_get` or `async_exchange`. [Protocol tuning](../guides/protocol-tuning.md) covers batches and handler
limits; the [HTTP/2 reference](../reference/http2/README.md) covers the TLS server overloads and flow control.

Next: [13. QUIC and HTTP/3](13-quic-and-http3.md)
