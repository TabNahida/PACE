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
pace::quic::listener listener(io, {"127.0.0.1", 0}, server_config);
```

Keep these objects outside the peer tasks, through `io.run()`. Paths above assume
the repository working directory and are public test credentials only. QUIC has
its own TLS 1.3 handshake; enabling the separate TCP TLS module is unnecessary.

## 2. Open a stream and explicitly end one direction

In the client coroutine:

```cpp
pace::quic::connection connection(io, client_config);
co_await connection.connect(endpoint, "localhost");
auto stream = co_await connection.open_stream();
co_await stream.write_all("hello");
stream.finish_write();
```

On the server, await `listener.accept()` and then `connection.accept_stream()`.
Read until that stream's EOF, write the reply, and call `finish_write()` for the
reply direction. FIN applies to the stream, not to every stream on the connection.
Keep the connection alive while its streams operate. `write_all` accepts data for
transmission; if you need to retain the connection until queued data is
acknowledged, await events while `pending_write_bytes()` is nonzero. Immediate
`connection.close()` is best-effort and is not a delivery confirmation.

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
pace::http3::limits limits;
limits.max_concurrent_handlers = 8;
io.spawn(pace::http3::serve(listener, app, limits));
```

Include `<pace/http3.hpp>` and `<pace/http_router.hpp>`. In the client, establish a
verified QUIC connection with the `h3` configuration, build a request vector as in
chapter 10, then transfer the connection:

```cpp
const auto replies = co_await pace::http3::exchange(
    std::move(connection), "localhost", std::move(requests));
```

Do not also open your own application/control streams on that transferred
connection. The adapter owns HTTP/3 control and QPACK streams. Results preserve
batch order; bodies are buffered and handler overlap has the same cooperative
semantics as HTTP/2. Normal disconnect does not forcibly destroy active handlers;
keep their captures valid until completion, and stop the context for cancellation.

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
