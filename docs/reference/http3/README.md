# HTTP/3

[简体中文](README.zh.md) · [Reference](../README.md)

## Overview

`pace::http3` is an HTTP/3 (RFC 9114) client and server over authenticated [QUIC](../quic/README.md) connections
with ALPN `h3`, using nghttp3 for framing and QPACK. Requests and responses are the protocol-independent
`http::request` and `http::response` of [`<pace/http.hpp>`](../http/README.md); bodies are buffered.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/http3.hpp>` | `pace_http3` | `--http3=y` (enables `--quic=y`) |

The module depends on nghttp3 1.18.0 and the QUIC module (either backend), and defines `PACE_HAS_HTTP3`. It does
not use the HTTP/1, HTTP/2 or TLS modules. Like QUIC it runs on POSIX only. With `--dns=y` the URL client resolves
host names; otherwise URLs need numeric addresses. The C++ named module is `pace.http3`.

The module has a batch client (`async_exchange`, `async_get`) that sends requests over a QUIC connection you have
established, and a URL client (`fetch`, `get`, `post` and their `async_` forms) that opens one connection per call.

## Synopsis

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

## Types

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

Per-connection bounds, applied to incoming and outgoing messages by clients and servers. Every function takes a
`limits` as its last argument.

- `max_header_bytes`: decoded header bytes per stream (name and value lengths plus 32 bytes per field). Initial
  headers and trailers share this allowance.
- `max_header_fields`: header fields per stream.
- `max_body_bytes`: buffered body per stream, in each direction. 0 allows only empty bodies.
- `max_concurrent_streams`: request streams open at once. It is also the largest client batch; the peer may allow
  fewer, in which case the client opens more streams as credit arrives.
- `max_requests_per_connection`: requests over the life of a connection.
- `max_concurrent_handlers`: server handler tasks running at once on one connection; the effective cap is
  `min(max_concurrent_handlers, max_concurrent_streams)`. 1 serializes handlers.

`max_header_bytes`, `max_header_fields` and `max_requests_per_connection` must be nonzero;
`max_concurrent_streams` and `max_concurrent_handlers` must be 1–10,000.

### Message fields on HTTP/3

The shared message types map to HTTP/3 as they do to [HTTP/2](../http2/README.md#types): received messages have
version `"HTTP/3"`, `authority` and `target` supply `:authority` and `:path`, generated framing replaces
`Content-Length` and `Host`, HTTP/1-only fields are ignored, trailers are supported, and HEAD, 204, 205 and 304
carry no body. `CONNECT` is not supported: the client rejects it and the server answers 405.

## Functions

### Batch client

```cpp
task<std::vector<http::response>> async_exchange(quic::connection transport, std::string authority,
                                                 std::vector<http::request> requests, limits bounds = {});
```

Send the requests of the batch on separate bidirectional streams and return the responses in request order. The
call owns `transport`, an established connection that negotiated ALPN `h3`, and closes it after the batch. The
driver services reads, writes and stream credit together. The batch must hold 1 to `max_concurrent_streams`
requests and fit `max_requests_per_connection`. A refused, reset, truncated or malformed stream fails the whole
call: there is no partial result and no retry.

```cpp
task<http::response> async_get(quic::connection transport, std::string authority, std::string target = "/",
                               limits bounds = {});
```

A batch of one `GET` of `target`. For other methods, custom headers or several requests, use `async_exchange`.

### Server

```cpp
task<void> serve_connection(quic::connection transport, http::handler handle, limits bounds = {});
```

Serve one established connection that negotiated `h3` until the peer finishes or the connection fails. Errors and
cancellation propagate.

```cpp
task<void> serve(quic::listener& listener, http::handler handle, limits bounds = {});
```

Accept connections on `listener` and serve each one until the listener is closed or the call is canceled; then
cancel and join every accepted connection and return normally. The listener's server context needs a certificate
and ALPN `{"h3"}`. A failed handshake or connection ends only itself; an engine failure propagates.

### URL client

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

Each call opens one QUIC connection, sends one request and returns its response. Only `https://` URLs are
accepted. Without a `quic::context` the call uses the system trust store and ALPN `h3`; a caller-supplied context
must set ALPN `{"h3"}` itself and outlive the call. The connection goes to the first resolved address only. These
calls share their URL, request and deadline rules with the other URL clients; see
[URL clients](../http/clients.md). The blocking forms run on a private context and throw `std::logic_error` when
called inside a PACE context.

## Errors

| Condition | Exception |
|:--|:--|
| Invalid `limits`, connection without ALPN `h3`, empty authority, empty or oversized batch, invalid request or outgoing header, `CONNECT`, non-`https` URL | `std::invalid_argument` |
| Headers or body over the limits | `std::length_error`; the connection fails |
| Peer protocol error, malformed message, refused, reset or incomplete stream, GOAWAY that refuses a request, excess peer streams | `http::protocol_error` |
| Cancellation, or the context stopped | `std::system_error` with `std::errc::operation_canceled` |
| QUIC failures | as listed for [QUIC](../quic/README.md#errors) |

A handler that throws produces a `500 Internal Server Error` response for its stream. `serve()` isolates accepted
connections; `serve_connection()` and `async_exchange()` propagate their errors.

## Threads and concurrency

- All calls run on the context that owns the QUIC connection. One driver coroutine per connection owns nghttp3
  and the QUIC streams and waits on QUIC events, never on fixed sleeps.
- Within one connection, completed requests run their handlers concurrently, up to
  `min(max_concurrent_handlers, max_concurrent_streams)`. Handler results enter a bounded queue and wake the
  driver explicitly. A handler waiting for asynchronous I/O does not block protocol progress or other streams;
  synchronous work in a handler blocks the context. Shared handler state must tolerate coroutine interleaving.
- Responses may complete out of order; batch results keep request order.

## Lifetime and ownership

- `async_exchange`, `async_get`, `serve_connection` and the URL client own their connection and close it before
  completing.
- `serve()` borrows `listener`, which must outlive the call. Closing the listener cancels and joins every accepted
  connection before `serve()` returns.
- Each connection opens its own control stream and QPACK encoder and decoder streams. Response bodies stay alive
  until QUIC acknowledges them.
- When a connection ends, queued requests are dropped and running handlers are canceled and joined before the call
  returns. PACE waits inside a handler observe that cancellation automatically; custom awaiters must take part in
  [cancellation](../runtime/structured-concurrency.md).

## Limits and defaults

| Setting | Default | Bounds |
|:--|--:|:--|
| `max_header_bytes` | 16 KiB per stream, headers and trailers together | nonzero |
| `max_header_fields` | 100 per stream | nonzero |
| `max_body_bytes` | 1 MiB per stream and direction | 0 = empty bodies only |
| `max_concurrent_streams` | 32 | 1–10,000 |
| `max_requests_per_connection` | 1,000 | nonzero |
| `max_concurrent_handlers` | 32 | 1–10,000 |
| QPACK dynamic table | 0 (static table and literals only) | |
| URL client trust | System store, ALPN `h3` | |

Peak memory grows with concurrent streams times body size.

Not provided: streaming bodies, CONNECT tunnels, server push, HTTP Datagrams, WebTransport and a reusable client
connection.

## See also

- [QUIC](../quic/README.md), the transport
- [HTTP messages and handlers](../http/README.md), [routing](../http/router.md), [URL clients](../http/clients.md)
- [HTTP/2](../http2/README.md), [protocol tuning](../../guides/protocol-tuning.md)
- Tutorial: [QUIC and HTTP/3](../../tutorial/13-quic-and-http3.md)
- [Runnable example](../../../examples/tutorial_http3.cpp)
- [RFC 9114](https://www.rfc-editor.org/rfc/rfc9114), [RFC 9204](https://www.rfc-editor.org/rfc/rfc9204),
  [nghttp3 programmer's guide](https://nghttp2.org/nghttp3/programmers-guide.html)
