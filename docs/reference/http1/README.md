# HTTP/1

[简体中文](README.zh.md) · [Reference](../README.md)

## Overview

HTTP/1.0 and HTTP/1.1 over TCP and TLS: a server, a persistent client, one-shot and URL clients, and the small
`http::app`. Content-Length and chunked framing, trailers, keep-alive and strict framing validation are built in.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/http1.hpp>` (content in `<pace/http1/protocol.hpp>`) | `pace_http1` | `--http1=y` (default) |

The named module is `pace.http1`. The names are in namespace `pace::http` (also reachable as `pace::http1`),
next to the protocol-independent [messages and router](../http/README.md) that the module uses. The HTTPS
overloads exist when the build also has `--tls=y` (`PACE_HAS_TLS`); host names in URLs need `--dns=y`.
`pace_http1` also contains the [gateway services](../gateway/README.md), and `<pace/http1.hpp>` declares them.

| Page | Contents |
|:--|:--|
| This page | How HTTP/1 writes and reads messages, `limits`, framing rules and errors shared by server and client |
| [Server](server.md) | `serve`, `serve_connection`, `server_options`, admission, streaming responses, deadlines |
| [Client](client.md) | `client`, `secure_client`, `client_options`, one-shot calls, URL calls, HTTPS overloads |
| [Application](app.md) | `http::app`: routes, mounts, proxies and running a server in a few lines |

HTTP/1 here means exchanges of complete messages. It does not implement `Expect: 100-continue`, `Upgrade`,
`CONNECT` tunnels, informational responses sent by the server, streamed request bodies or content-coding
(compression). [WebSocket](../websocket/README.md), [HTTP/2](../http2/README.md) and [HTTP/3](../http3/README.md)
have their own modules; this module neither negotiates nor upgrades to them.

## Synopsis

```cpp
#include <pace/http1.hpp>

#include <stdexcept>

pace::task<pace::http::response> hello(pace::http::request request)
{
    co_return pace::http::text("Hello from " + request.target + "\n");
}

pace::task<void> check(pace::io_context& io, pace::tcp::endpoint endpoint)
{
    auto reply = co_await pace::http::async_get(io, endpoint, "/PACE");
    io.stop();
    if (reply.body != "Hello from /PACE\n")
    {
        throw std::runtime_error("unexpected reply");
    }
}

int main()
{
    pace::io_context io;
    pace::tcp::listener listener(io, {"127.0.0.1", 0});
    io.spawn(pace::http::serve(listener, hello));
    io.spawn(check(io, listener.local_endpoint()));
    io.run();
}
```

## Types

### `limits`

```cpp
struct limits
{
    std::size_t max_header_bytes = std::size_t{16} * 1024;
    std::size_t max_body_bytes = std::size_t{1024} * 1024;
    std::size_t max_requests_per_connection = 1000;
    std::size_t max_header_fields = 100;
    std::size_t max_trailer_bytes = std::size_t{8} * 1024;
    std::size_t max_trailer_fields = 32;
    std::size_t max_chunk_line_bytes = 1024;
    std::size_t max_chunks = 65536;
    std::size_t max_informational_responses = 8;
};
```

Bounds on what a server or client receives. They do not limit messages the application builds itself, the total
memory, the number of connections or how long a connection lives (see the server's
[deadlines](server.md) for that).

| Member | Bounds | Role |
|:--|:--|:--|
| `max_header_bytes` | Start line and header fields, including the final empty line | Both |
| `max_body_bytes` | The de-framed body, whether framed by Content-Length, chunks or the end of the connection | Both |
| `max_requests_per_connection` | Requests served on one connection; the last permitted response closes it | Server |
| `max_header_fields` | Number of header fields | Both |
| `max_trailer_bytes` | Trailer field lines and the final empty line | Both |
| `max_trailer_fields` | Number of trailer fields | Both |
| `max_chunk_line_bytes` | One chunk-size line, including extensions and CRLF | Both |
| `max_chunks` | Number of chunks, including the terminating zero-size chunk | Both |
| `max_informational_responses` | 1xx responses skipped before the final response | Client |

A Content-Length larger than `max_body_bytes` is rejected before anything is allocated or read. Zero is not
"unlimited": a zero limit allows none of that resource, so `max_body_bytes = 0` allows only empty bodies and
`max_requests_per_connection = 0` closes every connection without serving it.

### How HTTP/1 uses `request` and `response`

The message types are described on the [HTTP page](../http/README.md#types). HTTP/1 applies these rules to them.

**Received messages.** `method` and `target` keep their wire spelling. `version` is `HTTP/1.0` or `HTTP/1.1`.
`body` holds the de-framed payload; chunk boundaries and chunk extensions are not kept. Trailers are stored in
`trailers` and never merged into `headers`. Duplicate fields stay separate. On a received response,
`content_type` is the first `Content-Type` value (or empty), `chunked` tells whether it arrived chunked, `version`
is the server's version and `close` tells whether the connection ends after it. A received response may contain
managed fields such as `Content-Length`; remove them before sending that value on as a server response.

**Written messages.** Field names must be HTTP tokens and values must not contain CR, LF, NUL or other control
characters except tab. `Content-Length`, `Transfer-Encoding`, `Connection` and `Trailer` are generated by PACE and
must not appear in `headers`; a server response must also not carry `Content-Type` in `headers` (use
`content_type`). A request must not carry `Expect` or `Upgrade`, and `CONNECT` is not sent: this API cannot carry
out those handshakes. An HTTP/1.1 request needs exactly one valid `Host` field; the one-shot and URL clients add
it.

- A request's `keep_alive` produces `Connection: keep-alive` or `Connection: close`. A response's `close` ends the
  connection after it.
- With `chunked = false` the body is sent with a Content-Length. With `chunked = true` it is sent as one data chunk
  (none for an empty body), the terminating chunk and the `trailers`. Chunking changes the framing only; the body
  is still a complete buffer.
- Trailers require `chunked = true`. PACE validates them and writes a `Trailer` field naming them. Fields that
  affect framing, routing, connection control, authentication, cookies or content handling are refused as
  trailers; ordinary fields such as `X-Checksum` are fine. A receiver also refuses forbidden trailers, and any
  field that `Connection` names, even when they were not declared.
- An HTTP/1.0 request gets a Content-Length response even when the handler asked for `chunked`; a response with
  trailers to an HTTP/1.0 request is invalid.
- The server always writes an `HTTP/1.1` status line with a standard reason phrase for common codes and
  `Response` otherwise.

**HEAD and bodyless statuses.** A reply to `HEAD` carries the framing fields the body would have had (the body's
Content-Length, or `representation_length` when set) and no body, chunks or trailers. 204 and 304 have no body and
no Content-Length or Transfer-Encoding; 205 has `Content-Length: 0` and no body. The client does not read a body
for `HEAD`, 204 or 304, and a large announced length on them does not count against `max_body_bytes`. It rejects
a 204 that has Content-Length or Transfer-Encoding and a 205 whose body is not empty.

**Informational responses.** The client skips up to `max_informational_responses` 1xx responses and fails on
more, and fails on `101 Switching Protocols`. Receiving `100 Continue` does not start an Expect handshake: the
client always sends the whole request before reading. The server never sends 1xx responses and answers every
request carrying `Expect` with 417.

## Functions

The functions of this module are documented on its pages: [`serve`, `serve_connection`](server.md);
[`async_get`, `async_fetch`, `async_post`, `get`, `post`, `fetch`](client.md); [`app`](app.md).

## Errors

Both sides validate framing strictly. A server answers bad input with the status below and closes the connection;
a client throws `http::protocol_error` and closes the connection.

| Input | Server reply |
|:--|:--|
| Malformed request line, header or chunk syntax; invalid chunk size or trailer | 400 |
| Duplicate Content-Length (even identical), invalid Content-Length | 400 |
| Both Transfer-Encoding and Content-Length; duplicate Transfer-Encoding fields | 400 |
| `Connection` naming `Host`, a framing field or `Trailer` | 400 |
| HTTP/1.1 request without `Host`, or with duplicate, empty or invalid `Host` | 400 |
| HTTP/1.0 request with Transfer-Encoding | 400 |
| Incomplete message at end of input | 400 when a reply is still possible |
| A transfer coding other than a single `chunked` | 501 |
| `CONNECT` request or `Upgrade` field | 501 |
| `Expect` field, including `100-continue` | 417 |
| Body or chunk count over the limit | 413 |
| Header, trailer or chunk-line bytes or field count over the limit | 431 |
| Version other than `HTTP/1.0` or `HTTP/1.1` | 505 |

Chunk extensions are checked (token or quoted-string values) and then ignored. `Host` is checked only for
syntax; virtual hosting, authentication and origin policy are the application's. Other errors (invalid
application input, transport errors, cancellation) are listed on the [server](server.md#errors) and
[client](client.md#errors) pages.

## Threads and concurrency

Every object of this module is bound to the context that runs it, and every operation runs on that context's
thread. A server runs the requests of one connection one after another; different connections run concurrently,
on different threads when the server runs on several workers, and share the handler's state. A client allows one
exchange at a time. Details are on the [server](server.md#threads-and-concurrency) and
[client](client.md#threads-and-concurrency) pages.

## Lifetime and ownership

Messages are owned values. A server owns each accepted connection and its copy of the handler; a client owns its
transport. Listeners, contexts and TLS configurations are borrowed and must outlive the tasks that use them. The
exact rules are on each page.

## Limits and defaults

| `limits` member | Default |
|:--|:--|
| `max_header_bytes` | 16 KiB |
| `max_body_bytes` | 1 MiB |
| `max_requests_per_connection` | 1000 |
| `max_header_fields` | 100 |
| `max_trailer_bytes` | 8 KiB |
| `max_trailer_fields` | 32 |
| `max_chunk_line_bytes` | 1024 bytes |
| `max_chunks` | 65536 |
| `max_informational_responses` | 8 |
| Server status range | 200–599 |

Server and client options have their own defaults, listed on the [server](server.md#limits-and-defaults) and
[client](client.md#limits-and-defaults) pages.

## See also

- [HTTP messages](../http/README.md), [routing](../http/router.md), [URL clients across protocols](../http/clients.md)
- [Gateway services](../gateway/README.md)
- [TLS](../tls/README.md), [TCP](../net/tcp.md)
- Tutorial: [HTTP client](../../tutorial/02-http-client.md), [HTTP server](../../tutorial/03-http-server.md);
  guide: [HTTP recipes](../../guides/http-recipes.md)
- [RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html), HTTP/1.1 message syntax
