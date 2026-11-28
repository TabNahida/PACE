# 13. QUIC and HTTP/3

[中文](13-quic-and-http3.zh.md) · [Learning path](README.md) · [Previous: HTTP/2](12-http2.md)

QUIC adds authenticated, reliable streams on top of UDP. HTTP/3 then adds HTTP framing and QPACK header compression
on those streams. Build one layer at a time: raw UDP datagrams are not QUIC, and arbitrary bytes on a QUIC stream are
not HTTP/3 messages.

Both modules are optional and need a POSIX platform. `--http3=y` also enables QUIC; xmake downloads OpenSSL,
ngtcp2 and nghttp3 the first time. `--quic_backend=ngtcp2` (the default) or `--quic_backend=openssl` selects the
QUIC implementation; everything in this chapter works with either.

## Start with one HTTP/3 URL

Configure with `--http3=y` and link `pace_http3`. With an HTTP/3 service whose certificate OpenSSL's default trust store accepts:

```cpp
#include <pace/http3.hpp>
#include <iostream>

int main()
{
    auto reply = pace::http3::get("https://127.0.0.1:9443/health");
    std::cout << reply.status << " " << reply.body;
}
```

HTTP/3 URLs are always `https://`. For a server with a test certificate, create a `quic::context`, load the test CA,
set ALPN `h3` and call `pace::http3::get(trust, url)`. In a coroutine, use `co_await pace::http3::async_get(url)`.
[URL clients](../reference/http/clients.md) covers trust and URL options. The rest of this chapter builds a local
service and shows the QUIC streams underneath; a URL request needs none of that setup.

## Configure identities and the application protocol

Configure with `--quic=y`, link `pace_quic` and include `<pace/quic.hpp>`. Create a server and a client
`quic::context`, with the test certificate, key and CA from [chapter 10](10-tls-and-dns.md), and set ALPN
`pace-demo` on both sides:

```cpp
pace::quic::context server_config(pace::quic::role::server), client_config;
server_config.use_certificate_chain("tests/certs/localhost.pem", "tests/certs/localhost-key.pem");
client_config.load_verify_file("tests/certs/ca.pem");
server_config.set_alpn_protocols({"pace-demo"});
client_config.set_alpn_protocols({"pace-demo"});
auto listener = pace::quic::listen(io, {"127.0.0.1", 0}, server_config);
```

Keep these objects outside the peer tasks, alive until `io.run()` returns. The paths are relative to the PACE
checkout, and the credentials are public test credentials. QUIC carries its own TLS 1.3 handshake; the separate TLS
module for TCP is not needed.

## Open a stream and end one direction explicitly

In the client coroutine:

```cpp
auto connection = co_await pace::quic::connect(client_config, endpoint, "localhost");
auto stream = co_await connection.open_stream();
co_await pace::write(stream, "hello");
stream.finish_write();
```

On the server, await `listener.accept()` and then `connection.accept_stream()`. Read until that stream ends, write
the reply, and call `finish_write()` for the reply direction:

```cpp
std::array<char, 64> bytes{};
std::string request;
while (auto count = co_await stream.read_some(pace::as_writable_bytes(bytes)))
{
    request.append(bytes.data(), count);
}
co_await stream.write_all(pace::as_bytes("Hello over verified QUIC!\n"));
stream.finish_write();
```

`read_some` fills a byte span, here the `char` array viewed through `pace::as_writable_bytes`, and returns 0 at the
peer's FIN. `write_all` borrows the bytes `pace::as_bytes` views; a string literal lives long enough. The end of
stream (FIN) applies to that one stream, not
to the connection's other streams. Keep the connection alive while its streams are in use. A completed
`pace::write` does not prove that the peer received the bytes; before closing a sender, follow the delivery and FIN
rules in [protocol tuning](../guides/protocol-tuning.md), as the companion below does.

The companion [`tutorial_quic.cpp`](../../examples/tutorial_quic.cpp) waits until the server's reply has been
acknowledged and until both peers have finished before stopping, and fails after five seconds. Configure the PACE
checkout with `--quic=y --examples=y` and run `xmake run tutorial_quic`; expect `Hello over verified QUIC!`. In your
own copy, try two streams, and check that a FIN on the first does not close the second.

## Let HTTP/3 own the protocol streams

Configure with `--http3=y`, link `pace_http3`, and change both ALPN lists to `h3`. The request, response and router
types from [chapter 3](03-http-server.md) still apply:

```cpp
pace::http::router app;
app.get("/health", [](pace::http::request) { return pace::http::response{.body = "ok"}; });
io.spawn(pace::http3::serve(listener, app));
```

Include `<pace/http3.hpp>` and `<pace/http.hpp>`. In the client, open a verified QUIC connection with the `h3`
configuration, then request one response:

```cpp
auto connection = co_await pace::quic::connect(client_config, endpoint, "localhost");
const auto reply = co_await pace::http3::async_get(std::move(connection), "localhost", "/health");
std::cout << reply.status << " " << reply.body << '\n';
```

`async_get` takes over the connection: do not open your own streams on it. HTTP/3 uses its own control and QPACK
streams. `async_get` returns one buffered response; for several requests on one connection, use `async_exchange`.
[Protocol tuning](../guides/protocol-tuning.md) covers batches, concurrency and lifetimes. The call runs on your
existing context and starts no background thread.

## Verify the layered version

From the PACE checkout:

```sh
xmake f -m release --http3=y --examples=y -y
xmake -y tutorial_quic tutorial_http3
xmake run tutorial_quic
xmake run tutorial_http3
```

The companion [`tutorial_http3.cpp`](../../examples/tutorial_http3.cpp) sends one GET with `http3::async_get`, then
`/one` and `/two` as a batch with `http3::async_exchange`, and prints `Hello over HTTP/3, /one!` and
`Hello over HTTP/3, /two!`. In your copy, change the ALPN on one side and check that the connection fails: reaching a
UDP port does not establish an authenticated HTTP/3 session. This API has no server push, CONNECT, WebTransport or
0-RTT. The [QUIC](../reference/quic/README.md) and [HTTP/3](../reference/http3/README.md) references give the limits
and ownership rules.

## Write a complete single-request service

Create `h3_server` and `h3_client` targets as in [chapter 5](05-tcp.md), both depending on `pace_http3`. The test
certificate paths are relative to the PACE checkout, so start both programs with `xmake run -w third_party/PACE`
from the `hello` directory, as in [chapter 10](10-tls-and-dns.md). First the server:

```cpp
#include <pace/http.hpp>
#include <pace/http3.hpp>
#include <pace/runtime.hpp>
#include <iostream>

pace::task<void> serve()
{
    pace::quic::context security(pace::quic::role::server);
    security.use_certificate_chain("tests/certs/localhost.pem", "tests/certs/localhost-key.pem");
    security.set_alpn_protocols({"h3"});
    auto listener = pace::quic::listen({"127.0.0.1", 9094}, security);
    pace::http::router routes;
    routes.get("/health", [] { return pace::http::text("ok\n"); });
    co_await pace::http3::serve_connection(co_await listener.accept(), routes);
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

The listener binds on the running context and stays alive through the connection. Now the client; it keeps
`localhost` as the certificate identity although the address is numeric:

```cpp
#include <pace/http3.hpp>
#include <pace/runtime.hpp>
#include <iostream>

pace::task<pace::http::response> get_health()
{
    pace::quic::context trust;
    trust.load_verify_file("tests/certs/ca.pem");
    trust.set_alpn_protocols({"h3"});
    auto connection = co_await pace::quic::connect(trust, {"127.0.0.1", 9094}, "localhost");
    co_return co_await pace::http3::async_get(std::move(connection), "localhost:9094", "/health");
}

int main()
{
    try
    {
        auto reply = pace::run(get_health);
        std::cout << reply.status << " " << reply.body;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

Start `xmake run -w third_party/PACE h3_server`, then `xmake run -w third_party/PACE h3_client` in another
terminal. Expect `200 ok` and both programs to exit. Start the server
again for the next request. To accept connections in a loop, replace
`serve_connection(co_await listener.accept(), routes)` with `serve(listener, routes)`. The test certificates are for
this local exercise only.

In the QUIC companion, the client stays alive after it has read the reply's FIN, until the server has seen its
acknowledgements: it keeps waiting for connection events until both peers have finished. Receiving a FIN does not
mean the remote sender is done.

Next: [14. Static files and a reverse proxy](14-gateway.md)
