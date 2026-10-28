# 11. From a QUIC stream to HTTP/3

[中文](11-quic-http3.zh-CN.md) · [Previous: HTTP/2](10-http2.md) · [Learning path](README.md)

QUIC adds authenticated, reliable streams above UDP. HTTP/3 then supplies HTTP
framing and QPACK on those streams. Build one layer at a time: raw UDP datagrams
are not QUIC, and arbitrary QUIC stream bytes are not HTTP/3 messages.
This backend currently requires POSIX.

## 1. Configure identities and the application protocol

Enable `--quic=y`, link `pace_quic`, and include `<pace/quic.hpp>`. Use server/client
`quic::context` objects, configuring the test chain/key and client trust as in the
TLS exercise, but set ALPN to `pace-demo` on both sides:

```cpp
pace::quic::context server_config(pace::quic::role::server), client_config;
server_config.use_certificate_chain("tests/certs/localhost.pem", "tests/certs/localhost-key.pem");
client_config.load_verify_file("tests/certs/ca.pem");
server_config.set_alpn_protocols({"pace-demo"});
client_config.set_alpn_protocols({"pace-demo"});
auto listener = pace::quic::listen(io, {"127.0.0.1", 0}, server_config);
```

Keep these objects outside the peer tasks, through `io.run()`. Paths above assume
the repository working directory and are public test credentials only. QUIC has
its own TLS 1.3 handshake; enabling the separate TCP TLS module is unnecessary.

## 2. Open a stream and explicitly end one direction

In the client coroutine:

```cpp
auto connection = co_await pace::quic::connect(client_config, endpoint, "localhost");
auto stream = co_await connection.open_stream();
co_await pace::write(stream, "hello");
stream.finish_write();
```

On the server, await `listener.accept()` and then `connection.accept_stream()`.
Read until that stream's EOF, write the reply, and call `finish_write()` for the
reply direction. FIN applies to the stream, not to every stream on the connection.
Keep the connection alive while its streams operate. Awaiting `pace::write`
is not proof of peer receipt. Before closing a sender, follow the
[delivery and FIN contract](../reference/protocol-tuning.md#quic), also used by
the companion below.

The [QUIC companion](../../examples/tutorial_quic.cpp) tracks both peers before
stopping and imposes a local deadline. Build/run `tutorial_quic`; expect
`Hello over verified QUIC!`. Try two streams in your own copy without treating a
FIN on the first as closure of the second.

## 3. Let HTTP/3 own the protocol streams

Enable `--http3=y` (which enables QUIC), link `pace_http3`, and change both ALPN
lists to `h3`. The HTTP request/response and router types from chapter 4 still work:

```cpp
pace::http::router app;
app.get("/health", [](pace::http::request) { return pace::http::response{.body = "ok"}; });
io.spawn(pace::http3::serve(listener, app));
```

Include `<pace/http3.hpp>` and `<pace/http_router.hpp>`. In the client, establish a
verified QUIC connection with the `h3` configuration, then request one response:

```cpp
auto connection = co_await pace::quic::connect(client_config, endpoint, "localhost");
const auto reply = co_await pace::http3::get(std::move(connection), "localhost", "/health");
std::cout << reply.status << " " << reply.body << '\n';
```

Do not also open your own application/control streams on that transferred
connection. The adapter owns HTTP/3 control and QPACK streams. `get` returns one buffered response. For several requests, use `exchange` instead;
[batching, concurrency and lifetime](../reference/protocol-tuning.md#http2-and-http3)
are covered in the reference. The helper uses your existing context and does not
start a background thread. These new helpers require the current checkout.

## 4. Verify the layered version

From the repository root:

```sh
xmake f --http3=y --examples=y -y
xmake -y tutorial_quic tutorial_http3
xmake run tutorial_quic
xmake run tutorial_http3
```

The [HTTP/3 companion](../../examples/tutorial_http3.cpp) prints responses for
`/one` and `/two`. In your copy change the peer ALPN on one side and verify failure;
a UDP socket being reachable does not establish an authenticated HTTP/3 session.
This API does not expose server push, CONNECT, WebTransport or 0-RTT. Continue with
the [QUIC](../reference/quic.md) and [HTTP/3](../reference/http3.md) references for
limits and ownership rather than inferring support from the transport name.

## 5. Write a complete single-request service

Create `h3_server.cpp` and `h3_client.cpp` targets as in chapter 3, both depending
on `pace_http3`. Enable `--http3=y`. Run these programs from the PACE checkout
root so the public test certificate paths resolve (or copy `tests/certs` into
your practice working directory). First write the server:

```cpp
#include <pace/http3.hpp>
#include <pace/http_router.hpp>
#include <pace/run.hpp>
#include <iostream>

pace::task<void> serve() {
    pace::quic::context security(pace::quic::role::server);
    security.use_certificate_chain("tests/certs/localhost.pem", "tests/certs/localhost-key.pem");
    security.set_alpn_protocols({"h3"});
    auto listener = pace::quic::listen({"127.0.0.1", 9094}, security);
    pace::http::router routes;
    routes.get("/health", [] { return pace::http::text("ok\n"); });
    co_await pace::http3::serve_connection(co_await listener.accept(), routes);
}
int main() {
    try { pace::run(serve); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

The listener binds on the current context and remains alive through the connection.
Now write the client; keep `localhost` as the certificate identity even though the
network address is numeric:

```cpp
#include <pace/http3.hpp>
#include <pace/run.hpp>
#include <iostream>

pace::task<pace::http::response> get_health() {
    pace::quic::context trust;
    trust.load_verify_file("tests/certs/ca.pem");
    trust.set_alpn_protocols({"h3"});
    auto connection = co_await pace::quic::connect(trust, {"127.0.0.1", 9094}, "localhost");
    co_return co_await pace::http3::get(std::move(connection), "localhost:9094", "/health");
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

Start `h3_server`, then `h3_client` in another terminal. Expect `200 ok` and both
processes to exit. Restart the server for the next request. To accept repeatedly,
replace `serve_connection(co_await listener.accept(), routes)` with
`serve(listener, routes)`. Test certificates are only for this local exercise.

In the local QUIC exchange, the client also remains alive after reading the reply
FIN until the server has observed its acknowledgments. The companion awaits
connection events until both peers finish; receiving a FIN alone is not a join
with the remote sender.
