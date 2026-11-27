# HTTP/2

[简体中文](README.zh.md) · [Reference](../README.md)

## Overview

`pace::http2` is an HTTP/2 client and server over cleartext TCP (prior knowledge) or verified TLS with ALPN `h2`,
built on nghttp2. Requests and responses are the protocol-independent `http::request` and `http::response` of
[`<pace/http.hpp>`](../http/README.md); bodies are buffered.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/http2.hpp>` | `pace_http2` | `--http2=y`; the TLS forms also need `--tls=y` |

The module depends on nghttp2 1.70.0 and defines `PACE_HAS_HTTP2`. It does not use the HTTP/1 module. With
`--dns=y` the URL client resolves host names; otherwise URLs need numeric addresses. The C++ named module is
`pace.http2`.

The module has two kinds of client: a batch client (`async_exchange`, `async_get`) that sends requests over a
connection you have opened, and a URL client (`fetch`, `get`, `post` and their `async_` forms) that opens one
connection per call.

## Synopsis

```cpp
#include <pace/http2.hpp>
#include <pace/runtime.hpp>

#include <iostream>

pace::task<pace::http::response> hello(pace::http::request request)
{
    co_return pace::http::text("Hello, " + request.target + "!");
}

int main()
{
    pace::run(
        [](pace::io_context& io) -> pace::task<void>
        {
            pace::tcp::listener listener(io, {"127.0.0.1", 0});
            co_await pace::with_task_group(
                [&](pace::task_group& group) -> pace::task<void>
                {
                    auto server = group.spawn(pace::http2::serve(listener, hello));

                    std::vector<pace::http::request> requests(2);
                    requests[0].method = "GET";
                    requests[0].target = "/one";
                    requests[1].method = "GET";
                    requests[1].target = "/two";
                    auto responses = co_await pace::http2::async_exchange(
                        co_await pace::tcp::connect(listener.local_endpoint()), "localhost", std::move(requests)
                    );
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
    std::size_t max_concurrent_streams = 100;
    std::size_t max_requests_per_connection = 1000;
    std::size_t max_concurrent_handlers = 100;
};
```

Per-connection bounds, used by clients and servers. Every function takes a `limits` as its last argument.

- `max_header_bytes`: header bytes per stream, counted as in HTTP/2's header-list size (name and value lengths plus
  32 bytes per field). Initial headers, informational headers and trailers share this allowance.
- `max_header_fields`: header fields per stream.
- `max_body_bytes`: buffered body per stream, in each direction. 0 allows only empty bodies; it never means
  unlimited.
- `max_concurrent_streams`: streams open at once. It is also the largest client batch.
- `max_requests_per_connection`: requests over the life of a connection.
- `max_concurrent_handlers`: server handler tasks running at once on one connection; the effective cap is
  `min(max_concurrent_handlers, max_concurrent_streams)`. 1 serializes handlers.

`max_header_bytes`, `max_header_fields` and `max_requests_per_connection` must be nonzero;
`max_concurrent_streams` and `max_concurrent_handlers` must be 1–10,000. A bound violation by the peer fails the
connection.

### Message fields on HTTP/2

`http::request` and `http::response` are shared with HTTP/1.1 and HTTP/3. On HTTP/2:

- Received messages have `version` `"HTTP/2"`. Header names are sent in lowercase.
- The `authority` argument (or the URL) supplies `:authority`; the request's `target` supplies `:path`. Targets
  start with `/`; only `OPTIONS` may use `*`. `CONNECT` is not supported: the client rejects it and the server
  answers 405.
- `Content-Length` and `Host` fields in `headers` are replaced by generated framing and the authority. A request's
  `Content-Type` header is kept; a response's comes from `response.content_type`.
- Connection-specific fields (`Connection`, `Keep-Alive`, `Proxy-Connection`, `Transfer-Encoding`, `Upgrade`) and a
  `TE` other than `trailers` are rejected.
- `request.keep_alive`, `request.chunked`, `response.close`, `response.chunked` and an outgoing `version` are
  ignored: HTTP/2 has its own framing and stream lifecycle. In particular `response.close` does not close the
  connection.
- Trailers are sent and received. HEAD requests and 204, 205 and 304 responses carry no body.

## Functions

### Batch client

```cpp
task<std::vector<http::response>> async_exchange(tcp::stream transport, std::string authority,
                                                 std::vector<http::request> requests, limits bounds = {});
task<std::vector<http::response>> async_exchange(tls::stream transport, std::string authority,
                                                 std::vector<http::request> requests, limits bounds = {});
```

Send every request of the batch at once, each on its own stream, and return the responses in request order,
whatever order the streams complete in. The call owns `transport` and closes it after the batch. `authority` is
the HTTP authority (host and optional port), independent of the address the transport is connected to. The batch
must hold 1 to `max_concurrent_streams` requests and fit `max_requests_per_connection`. If any stream fails, is
reset or is incomplete, the whole call throws: there is no partial result and no retry.

The TCP form speaks HTTP/2 prior knowledge: it starts with the connection preface, with no HTTP/1.1 `Upgrade: h2c`.
The TLS form (`--tls=y`) needs a stream whose verified handshake negotiated ALPN `h2`: configure the client
`tls::context` with `set_alpn_protocols({"h2"})` before connecting.

```cpp
template <class Transport> // tcp::stream, or tls::stream with --tls=y
task<http::response> async_get(Transport transport, std::string authority, std::string target = "/",
                               limits bounds = {});
```

A batch of one `GET` of `target`. Returns that response; the connection is closed afterwards. For other methods,
custom headers or several requests, use `async_exchange`.

### Server

```cpp
task<void> serve_connection(tcp::stream transport, http::handler handle, limits bounds = {});
task<void> serve_connection(tls::stream transport, http::handler handle, limits bounds = {});
```

Serve one connection until the peer finishes or the connection fails. The call owns `transport`. The TLS form
needs a completed handshake that negotiated `h2`. Errors and cancellation propagate to the caller.

```cpp
task<void> serve(tcp::listener& listener, http::handler handle, limits bounds = {});
task<void> serve(tcp::listener& listener, const tls::context& configuration, http::handler handle,
                 limits bounds = {});
```

Accept connections and serve each one until `listener` is closed or the call is canceled; then cancel and join
every accepted connection and return normally. A failing connection (or, in the TLS form, a failed handshake) ends
only itself. The TLS form performs each handshake with `configuration`, which must have a certificate and ALPN
`{"h2"}`.

`http::handler` is `std::function<task<http::response>(http::request)>`; see [routing](../http/router.md) for
building one from a router.

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

// With --tls=y: the caller's TLS configuration, for https:// only.
task<http::response> async_fetch(io_context&, const tls::context&, std::string url, http::request,
                                 limits bounds = {});
task<http::response> async_get(io_context& io, const tls::context& trust, std::string url, limits bounds = {});
http::response fetch(const tls::context& trust, std::string url, http::request request,
                     blocking_options wait = {}, limits bounds = {});
http::response get(const tls::context& trust, std::string url, blocking_options wait = {}, limits bounds = {});
```

Each call opens one connection, sends one request and returns its response. `http://` URLs use prior knowledge;
`https://` URLs use TLS with the system trust store and ALPN `h2` (an `https://` URL without `--tls=y` throws
`std::system_error` with `std::errc::not_supported`). A caller-supplied `tls::context` must set ALPN `{"h2"}`
itself and outlive the call. These calls share their URL, request and deadline rules with the HTTP/1 and HTTP/3
URL clients; see [URL clients](../http/clients.md). The blocking forms run on a private context and throw
`std::logic_error` when called inside a PACE context.

## Errors

| Condition | Exception |
|:--|:--|
| Invalid `limits`, empty authority, empty or oversized batch, invalid request or outgoing header, `CONNECT` | `std::invalid_argument` |
| Outgoing headers over the limits | `std::length_error` |
| TLS transport without ALPN `h2`, peer protocol error, reset or incomplete stream, GOAWAY that refuses a request | `std::runtime_error` |
| Incoming headers or body over the limits | `std::length_error`; the connection fails |
| Server: request truncated by end of input, or response output blocked after it | `http::protocol_error` |
| Call from another context | `std::logic_error` |
| Cancellation, or the context stopped | `std::system_error` with `std::errc::operation_canceled` |
| Socket and TLS errors | `std::system_error` from the transport |

A handler that throws produces a `500 Internal Server Error` response for its stream; the connection continues.
Cancellation of a handler is not turned into a response. `serve()` isolates accepted connections, so these
errors never reach its caller; `serve_connection()` and `async_exchange()` propagate them.

## Threads and concurrency

- All calls run on the context that owns the transport; one connection is driven by one coroutine that owns the
  nghttp2 session and the transport.
- Within one connection, completed requests run their handlers concurrently, up to
  `min(max_concurrent_handlers, max_concurrent_streams)`. A handler suspended on asynchronous I/O does not block
  other streams or protocol progress, and handler completions wake the driver without polling. This is
  cooperative concurrency on one context, not parallel CPU work: a handler doing synchronous work blocks the
  context. Handler state shared between requests must tolerate coroutine interleaving.
- Ready requests wait within the stream limit while handler slots are busy; there is no unbounded queue. Responses
  may complete out of order.
- After a clean TCP end of input, already complete requests still run and their responses use the peer's remaining
  flow-control credit; truncated requests fail the connection, and a response that would need more credit fails
  instead of waiting.

## Lifetime and ownership

- `async_exchange`, `async_get`, `serve_connection` and the URL client own their transport and close it before
  completing.
- `serve()` borrows `listener` and, in the TLS form, `configuration`: both must outlive the call. Closing the
  listener cancels and joins every accepted connection before `serve()` returns; stopping the context also cancels
  unrelated work on it.
- When a connection ends, queued requests are dropped and running handlers are canceled and joined before the
  call returns. PACE waits inside a handler observe that cancellation automatically; custom awaiters must take
  part in [cancellation](../runtime/structured-concurrency.md). Anything a handler captures must stay valid until
  then.
- A reset stream discards its pending response, but its handler keeps its slot until it finishes.

## Limits and defaults

| Setting | Default | Bounds |
|:--|--:|:--|
| `max_header_bytes` | 16 KiB per stream, headers and trailers together | nonzero |
| `max_header_fields` | 100 per stream | nonzero |
| `max_body_bytes` | 1 MiB per stream and direction | 0 = empty bodies only |
| `max_concurrent_streams` | 100 | 1–10,000 |
| `max_requests_per_connection` | 1,000 | nonzero |
| `max_concurrent_handlers` | 100 | 1–10,000 |
| URL client `https://` trust | System store, ALPN `h2` | |
| TLS staging buffers (HTTP/2 over TLS) | 64 KiB out + 64 KiB in per connection | |
| Plaintext per TLS write | at most 16 KiB | |

Peak memory grows with active streams times body size; choose the limits for your concurrency.

Not provided: HTTP/1.1 `Upgrade: h2c`, CONNECT tunnels (including WebSocket over HTTP/2), server push, streaming
bodies and a reusable client connection.

## See also

- [HTTP messages and handlers](../http/README.md), [routing](../http/router.md), [URL clients](../http/clients.md)
- [HTTP/1](../http1/README.md), [HTTP/3](../http3/README.md), [TLS](../tls/README.md)
- [Protocol tuning](../../guides/protocol-tuning.md)
- Tutorial: [HTTP/2](../../tutorial/12-http2.md)
- [Runnable example](../../../examples/tutorial_http2.cpp)
- [RFC 9113](https://www.rfc-editor.org/rfc/rfc9113)
