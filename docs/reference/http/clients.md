# HTTP URL clients

[简体中文](clients.zh.md) · [HTTP](README.md)

## Overview

One-shot HTTP calls that take an absolute `http://` or `https://` URL and return the complete response. HTTP/1,
HTTP/2 and HTTP/3 each provide the same six names with the same argument order and the same ownership rules;
the protocol is chosen by the namespace.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/http1.hpp>` (namespace `pace::http`, alias `pace::http1`) | `pace_http1` | `--http1=y` (default) |
| `<pace/http2.hpp>` (namespace `pace::http2`) | `pace_http2` | `--http2=y` |
| `<pace/http3.hpp>` (namespace `pace::http3`) | `pace_http3` | `--http3=y` |

`https://` needs `--tls=y` for HTTP/1 and HTTP/2 (HTTP/3 always uses QUIC's TLS), and URLs with a host name need
`--dns=y`. URLs with a numeric IPv4 or IPv6 address need neither.

| Operation | Direct call, blocks this thread | Coroutine call, on a running context |
|:--|:--|:--|
| GET | `get(url)` | `co_await async_get(url)` |
| POST | `post(url, body, content_type)` | `co_await async_post(url, body, content_type)` |
| Any request | `fetch(url, request)` | `co_await async_fetch(url, request)` |

Each call opens its own connection, sends one request, reads one response and closes the connection. There is
no redirect following, retry, proxy, cookie store, decompression or connection reuse between calls. For several
requests on one connection, use the HTTP/1 [`client`](../http1/client.md), or `async_exchange` on an
[HTTP/2](../http2/README.md) or [HTTP/3](../http3/README.md) connection.

## Synopsis

```cpp
#include <pace/http1.hpp>

#include <iostream>

pace::task<void> report(std::string url)
{
    auto reply = co_await pace::http::async_get(std::move(url));
    std::cout << reply.status << ' ' << reply.body.size() << " bytes\n";
}

int main()
{
    // Direct call: its own context and connection, 30-second total deadline by default.
    auto reply = pace::http::get("http://127.0.0.1:8080/health");
    std::cout << reply.status << '\n';

    // Coroutine call: runs on the current context and inherits its cancellation and deadline.
    pace::run([] { return report("http://127.0.0.1:8080/health"); });
}
```

## Functions

The signatures below are those of HTTP/1. `limits` is the protocol's own limits type (`http::limits`,
`http2::limits`, `http3::limits`); `client_options` exists only on HTTP/1.

### Direct calls

```cpp
response get(std::string url, blocking_options wait = {}, limits bounds = {}, client_options options = {});
response post(
    std::string url,
    std::string body,
    std::string content_type = "application/octet-stream",
    blocking_options wait = {},
    limits bounds = {},
    client_options options = {}
);
response fetch(
    std::string url,
    request outgoing,
    blocking_options wait = {},
    limits bounds = {},
    client_options options = {}
);
```

Each creates a fresh `io_context` on the calling thread, runs the coroutine call on it and returns the owned
response. `wait` is the total budget of the call; see [`blocking_options`](../runtime/run.md). No helper thread
and no shared global loop is involved. A direct call must not be made from inside a PACE task or callback.

### Coroutine calls

```cpp
task<response> async_get(io_context& context, std::string url, limits bounds = {}, client_options options = {});
task<response> async_post(
    io_context& context,
    std::string url,
    std::string body,
    std::string content_type = "application/octet-stream",
    limits bounds = {},
    client_options options = {}
);
task<response> async_fetch(
    io_context& context,
    std::string url,
    request outgoing,
    limits bounds = {},
    client_options options = {}
);

task<response> async_get(std::string url, limits bounds = {}, client_options options = {});
task<response> async_post(
    std::string url,
    std::string body,
    std::string content_type = "application/octet-stream",
    limits bounds = {},
    client_options options = {}
);
task<response> async_fetch(std::string url, request outgoing, limits bounds = {}, client_options options = {});
```

The forms without `io_context&` use the context that runs the task, found when the task starts. The returned task
is lazy: nothing, not even URL parsing, happens until it is awaited or spawned. It owns all its arguments.

`async_get` sends `GET`. `async_post` sends `POST` with `body` and a `Content-Type` field of `content_type`.
`async_fetch` sends `outgoing` as given (method, headers, body, trailers), except that the URL decides the target
and the authority.

### Calls with an explicit trust configuration

```cpp
// HTTP/1 and HTTP/2, with --tls=y: Trust is tls::context. HTTP/3: Trust is quic::context.
response get(const Trust& trust, std::string url, blocking_options wait = {}, limits bounds = {});
response fetch(const Trust& trust, std::string url, request outgoing, blocking_options wait = {}, limits bounds = {});
task<response> async_get(io_context& context, const Trust& trust, std::string url, limits bounds = {});
task<response> async_fetch(
    io_context& context,
    const Trust& trust,
    std::string url,
    request outgoing,
    limits bounds = {}
);
```

HTTP/1 adds `client_options options = {}` after `bounds` here too. The URL must be `https://`. The configuration
replaces the default one: it chooses the trusted roots or another
[verification mode](../tls/README.md), and its ALPN list must offer the protocol (`http/1.1`, `h2`, `h3`). It is
borrowed, not copied, and must stay alive until the call completes. There is no `post` or context-free `async_`
form with a trust argument; for a POST, fill `request` (method, body, `Content-Type` header) and call `fetch`.

```cpp
pace::tls::context trust;
trust.load_verify_file("ca.pem");
trust.set_alpn_protocols({"http/1.1"});
auto reply = pace::http::get(trust, "https://localhost:8443/health");
```

### URL rules

All three protocols parse URLs the same way. This is an HTTP client subset, not a general URI parser.

- The scheme is `http` or `https`, in any case, followed by `://`.
- The host is a numeric IPv4 address, a bracketed IPv6 address (`[::1]`) or an ASCII DNS name (labels of 1–63
  letters, digits and `-`, not starting or ending with `-`; at most 253 characters).
- An optional port is a decimal number from 1 to 65535. The default is 80 for `http`, 443 for `https`.
- The path and query are sent as written; an empty path becomes `/`, a query without a path becomes `/?…`.
  The fragment is dropped.
- Rejected: userinfo (`user@`), spaces, control characters, backslashes, non-ASCII characters, percent-encoded
  hosts, IPv6 zone identifiers and unbracketed IPv6 addresses. Encode non-ASCII path and query bytes yourself;
  existing percent escapes are kept as they are.

The authority, exactly as written in the URL, becomes the `Host` field (`:authority` on HTTP/2 and HTTP/3). The
host part is also the name the server certificate must match and the SNI name. The request must not contain a
`Host` field, nor on HTTP/2 and HTTP/3 any `:`-prefixed field.

### What differs between the protocols

| | HTTP/1 | HTTP/2 | HTTP/3 |
|:--|:--|:--|:--|
| `http://` | HTTP/1.1 over TCP | HTTP/2 over TCP with prior knowledge | Rejected |
| `https://` | TLS, ALPN `http/1.1` (or none) | TLS, ALPN `h2` | QUIC, ALPN `h3` |
| Trust type | `tls::context` | `tls::context` | `quic::context` |
| Resolved addresses | Tried in order until a TCP connection succeeds | Tried in order until a TCP connection succeeds | Only the first is used |
| Extra parameter | `client_options` (close policy) | none | none |
| Malformed response | `http::protocol_error` | See [HTTP/2](../http2/README.md) | See [HTTP/3](../http3/README.md) |
| Platform | All | All | POSIX |

The HTTP/1 call sends `Connection: close`, keeps the request's `version` (empty means `HTTP/1.1`) and finishes
as soon as the response is complete, without waiting for the server to close; see
[`client_options`](../http1/client.md). An HTTP/1 response with no length is read until the server closes.

## Errors

| Situation | Exception |
|:--|:--|
| Malformed URL, `Host` (or pseudo-header) in the request, `http://` with a trust argument, `http://` on HTTP/3 | `std::invalid_argument` |
| `https://` without TLS support, host name without DNS support | `std::system_error` with `std::errc::not_supported` |
| DNS lookup failed or returned no address | `std::system_error` from the resolver, or `std::errc::host_unreachable` |
| Connection, TLS or QUIC failure | `std::system_error` (the last address's error when several were tried) |
| Malformed, truncated or oversized response | The protocol's error (`http::protocol_error` on HTTP/1) |
| Cancellation | `std::system_error` with `std::errc::operation_canceled` |
| Direct call: deadline reached | `std::system_error` comparing equal to `std::errc::timed_out` |
| Direct call: made inside a running PACE context | `std::logic_error`, before any I/O |
| Direct call: `wait.timeout` zero or negative | `std::invalid_argument` |
| HTTP/1: negative `client_options::close_timeout` | `std::invalid_argument` |

A 4xx or 5xx status is a normal response, not an exception. Nothing is retried: once a connection exists, a
failure is reported even if the request may have reached the server.

## Threads and concurrency

A coroutine call runs entirely on one context and inherits the cancellation token and deadline of the task that
awaits it. It has no deadline of its own; bound it with `with_deadline` or `with_options`
([structured concurrency](../runtime/structured-concurrency.md)). Any number of calls may run concurrently on the
same or different contexts; each has its own connection.

A direct call blocks the calling thread until the response or the deadline. It cannot run inside a PACE context,
because it would block that context's event loop. Other event loops (a GUI, another library) are not detected
and are simply blocked. Its `wait.cancellation` token may be cancelled from another thread.

## Lifetime and ownership

The coroutine calls own their URL, body, content type and request. The `io_context&` and a trust configuration
are borrowed and must outlive the task. The returned `response` owns its data. A direct call owns everything for
its duration and leaves nothing running after it returns or throws.

## Limits and defaults

| Item | Default |
|:--|:--|
| `blocking_options::timeout` (direct calls) | 30 s total, covering DNS, connect, TLS, the exchange and close |
| Deadline of coroutine calls | None beyond the caller's |
| `content_type` of `post` | `application/octet-stream` |
| Default trust | System roots with certificate and host name verification |
| Default ALPN | `http/1.1`, `h2` or `h3` |
| Response size | The protocol's `limits` (`max_body_bytes` 1 MiB, `max_header_bytes` 16 KiB by default) |
| DNS deadline and retries | The resolver's defaults |
| TCP connect timeout | The operating system's |

## See also

- [HTTP messages](README.md)
- [HTTP/1 client](../http1/client.md), [HTTP/2](../http2/README.md), [HTTP/3](../http3/README.md)
- [`pace::run` and `blocking_options`](../runtime/run.md), [structured concurrency](../runtime/structured-concurrency.md)
- [TLS](../tls/README.md), [DNS](../dns/README.md)
- Tutorial: [HTTP client](../../tutorial/02-http-client.md); guide: [HTTP recipes](../../guides/http-recipes.md)
