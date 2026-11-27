# HTTP/1 client

[简体中文](client.zh.md) · [HTTP/1](README.md)

## Overview

Three ways to send HTTP/1 requests: a persistent `client` (or `secure_client` over TLS) that sends requests one
after another on a connection you opened; one-shot calls to a numeric endpoint; and URL calls, in a coroutine
(`async_get`, `async_post`, `async_fetch`) or as a direct blocking call (`get`, `post`, `fetch`).

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/http1.hpp>` | `pace_http1` | `--http1=y` (default); `--tls=y` for `secure_client`, `https://` and the `tls::context` overloads; `--dns=y` for host names in URLs |

Responses are buffered within the [limits](README.md#limits); a 4xx or 5xx status is a normal response. No call
follows redirects, retries, uses a proxy, stores cookies or decompresses. The URL calls follow the
[rules shared by HTTP/1, HTTP/2 and HTTP/3](../http/clients.md).

## Synopsis

```cpp
#include <pace/http1.hpp>

#include <iostream>

pace::task<void> two_requests(pace::io_context& io, pace::tcp::endpoint server)
{
    // A persistent connection: requests are sent one after another.
    pace::http::client client(co_await pace::tcp::connect(io, server));
    for (const char* target : {"/a", "/b"})
    {
        auto reply = co_await client.send({.method = "GET", .target = target, .headers = {{"Host", "localhost"}}});
        std::cout << target << ": " << reply.status << '\n';
    }

    // One-shot calls open and close their own connection.
    auto health = co_await pace::http::async_get(io, server, "/health");
    auto posted = co_await pace::http::async_post(io, "http://127.0.0.1:8080/items", R"({"id":1})", "application/json");
    std::cout << health.status << ' ' << posted.status << '\n';
}

int main()
{
    // A direct call blocks this thread and needs no running context.
    auto reply = pace::http::get("http://127.0.0.1:8080/health");
    std::cout << reply.body;
}
```

## Types

### `basic_client`, `client`, `secure_client`

```cpp
template <class Transport>
class basic_client
{
public:
    explicit basic_client(Transport stream, limits bounds = {}, client_options options = {});
    basic_client(const basic_client&) = delete;
    basic_client& operator=(const basic_client&) = delete;
    ~basic_client() = default;
    task<response> send(request outgoing);

    [[nodiscard]] Transport& transport() noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    void close() noexcept;
};

using client = basic_client<tcp::stream>;
using secure_client = basic_client<tls::stream>;  // with PACE_HAS_TLS
```

A persistent HTTP/1 connection. Only these two instantiations exist. The client is neither copyable nor movable.

| Member | Effect |
|:--|:--|
| Constructor | Takes ownership of a connected `tcp::stream`, or of a `tls::stream` whose handshake has completed. Throws `std::invalid_argument` if `options` is invalid |
| `send(outgoing)` | Validates and writes `outgoing`, then reads one final response. Bytes received after it are kept for the next exchange |
| `transport()` | The owned stream, for configuration while no exchange runs. Do not read or write it directly |
| `is_open()` | Whether the stream is open and the connection may be reused. It does not probe the peer |
| `close()` | Closes the stream at once; idempotent. Call it on the owning context |

`send` writes the request as described on the [HTTP/1 page](README.md#how-http1-uses-request-and-response). An
HTTP/1.1 request must carry its own `Host`. With an ALPN-capable `tls::stream`, the negotiated protocol must be
`http/1.1` or none. The client sends the whole request before reading; it does not pipeline.

After a response with Content-Length or chunked framing, the connection stays usable unless either side asked to
close (`request::keep_alive = false`, or a response with `close == true`). A response with no framing is read until
the server closes and ends the connection. When the connection ends after a complete response, `client_options`
decides how it is closed. When anything fails before the response is complete, the connection is closed at once.

### `client_close_policy`, `client_options`

```cpp
enum class client_close_policy
{
    no_wait,
    graceful
};

struct client_options
{
    client_close_policy close_policy = client_close_policy::no_wait;
    std::chrono::milliseconds close_timeout{100};
};
```

How a client closes the connection after the final response of that connection.

- `no_wait` sends the local TCP FIN or TLS `close_notify` without waiting for the peer and releases the stream. A
  transport error during this step does not replace the response that was already received; cancellation still
  propagates. `close_timeout` is not used.
- `graceful` waits for the peer to close, at most `close_timeout`; zero sends the close notification once and does
  not wait. Errors and cancellation during the close propagate.

Neither policy shortens a response framed by the end of the connection: it still needs the TCP EOF, or over TLS an
authenticated `close_notify`; a TLS connection that ends without one is a truncation error. The policy applies
only to closing, never to requests or response reads, and never to exchanges that keep the connection open.

```cpp
pace::http::client client(
    std::move(connected_socket),
    {},
    {.close_policy = pace::http::client_close_policy::graceful, .close_timeout = std::chrono::milliseconds(25)}
);
```

## Functions

### One-shot calls to an endpoint

```cpp
task<response> async_fetch(
    io_context& context,
    tcp::endpoint endpoint,
    request outgoing,
    limits bounds = {},
    client_options options = {}
);
task<response> async_get(
    io_context& context,
    tcp::endpoint endpoint,
    std::string target = "/",
    limits bounds = {},
    client_options options = {}
);
// With PACE_HAS_TLS:
task<response> async_fetch(
    io_context& context,
    const tls::context& configuration,
    tcp::endpoint endpoint,
    std::string peer_name,
    request outgoing,
    limits bounds = {},
    client_options options = {}
);
task<response> async_get(
    io_context& context,
    const tls::context& configuration,
    tcp::endpoint endpoint,
    std::string peer_name,
    std::string target = "/",
    limits bounds = {},
    client_options options = {}
);
```

Connect to a numeric `endpoint`, send one request with `Connection: close`, read the response and close. There is
no DNS lookup and no URL parsing. `async_get` sends `GET target`; `async_fetch` sends `outgoing` with `keep_alive`
forced to `false`.

When the request has no `Host`, one is added: the endpoint address for HTTP, `peer_name` for HTTPS, with the port
appended when it is not 80 (HTTP) or 443 (HTTPS), and IPv6 addresses in brackets (`[::1]:8080`). A `Host` you
supply is kept. Over TLS, `peer_name` is the name the certificate must match and the SNI name; `endpoint` only
says where to connect. `configuration` chooses the trust and [verification mode](../tls/README.md); give it ALPN
`http/1.1`, or no ALPN.

```cpp
pace::tls::context trust;
trust.load_verify_file("ca.pem");
auto reply = co_await pace::http::async_get(io, trust, {"127.0.0.1", 8443}, "localhost", "/health");
```

### URL calls in a coroutine

```cpp
task<response> async_fetch(
    io_context& context,
    std::string url,
    request outgoing,
    limits bounds = {},
    client_options options = {}
);
task<response> async_get(io_context& context, std::string url, limits bounds = {}, client_options options = {});
task<response> async_post(
    io_context& context,
    std::string url,
    std::string body,
    std::string content_type = "application/octet-stream",
    limits bounds = {},
    client_options options = {}
);
task<response> async_fetch(std::string url, request outgoing, limits bounds = {}, client_options options = {});
task<response> async_get(std::string url, limits bounds = {}, client_options options = {});
task<response> async_post(
    std::string url,
    std::string body,
    std::string content_type = "application/octet-stream",
    limits bounds = {},
    client_options options = {}
);
// With PACE_HAS_TLS:
task<response> async_fetch(
    io_context& context,
    const tls::context& configuration,
    std::string url,
    request outgoing,
    limits bounds = {},
    client_options options = {}
);
task<response> async_get(
    io_context& context,
    const tls::context& configuration,
    std::string url,
    limits bounds = {},
    client_options options = {}
);
```

Parse `url`, resolve its host if it is a name, connect, send one request and return the response. Everything
happens when the task runs, not when it is created. The URL decides the target and the `Host`; the request must
not contain a `Host` field. `async_post` sends `body` with a `Content-Type` of `content_type`. The forms without
`io_context&` use the context that runs them.

`https://` URLs use a default `tls::context` that verifies the server against the system roots and offers ALPN
`http/1.1`. The `configuration` overloads use yours instead; the URL must then be `https://`, and the URL host is
still the certificate name and the SNI name, even when it is a numeric address. Resolved addresses are tried in
order until one TCP connection succeeds; after that, nothing is retried. URL syntax and the remaining rules are on
the [URL clients page](../http/clients.md#url-rules).

### Direct calls

```cpp
inline response fetch(
    std::string url,
    request outgoing,
    blocking_options wait = {},
    limits bounds = {},
    client_options options = {}
);
inline response get(std::string url, blocking_options wait = {}, limits bounds = {}, client_options options = {});
inline response post(
    std::string url,
    std::string body,
    std::string content_type = "application/octet-stream",
    blocking_options wait = {},
    limits bounds = {},
    client_options options = {}
);
// With PACE_HAS_TLS:
inline response fetch(
    const tls::context& configuration,
    std::string url,
    request outgoing,
    blocking_options wait = {},
    limits bounds = {},
    client_options options = {}
);
inline response get(
    const tls::context& configuration,
    std::string url,
    blocking_options wait = {},
    limits bounds = {},
    client_options options = {}
);
```

Run the matching URL call on a fresh `io_context` on the calling thread and return its response. `wait` bounds the
whole call (DNS, connect, TLS, exchange and close); its `timeout` must be positive, and its `deadline` and
`cancellation` may shorten it ([`blocking_options`](../runtime/run.md)). There is no `post` with a TLS
configuration: fill a `request` with method, body and `Content-Type` and call `fetch`.

## Errors

| Situation | Exception |
|:--|:--|
| Malformed, truncated or over-limit response | `http::protocol_error`; the connection is closed |
| Invalid request (method, target, version, field names or values, managed fields, `Expect`, `Upgrade`, `CONNECT`, missing `Host` on HTTP/1.1, trailers without `chunked`) | `std::invalid_argument` before anything is written; a persistent client stays usable |
| `send` while another `send` is running, after the connection closed, or from another context | `std::logic_error` |
| Invalid `client_options` (unknown policy, negative `close_timeout`) | `std::invalid_argument` from the constructor or the one-shot call |
| Transport or TLS error, cancellation | `std::system_error`; the connection is closed |
| TLS peer negotiated an ALPN protocol other than `http/1.1` | `std::system_error` with `std::errc::protocol_error` |
| URL call errors (bad URL, `Host` supplied, missing TLS or DNS support) | See the [URL clients page](../http/clients.md#errors) |
| Direct call inside a running context, non-positive timeout, deadline reached | `std::logic_error`, `std::invalid_argument`, `std::system_error` with `std::errc::timed_out` |

## Threads and concurrency

A `basic_client` belongs to its stream's context: construct it there or before that context runs, call `send` from
a task on that context, and call `close` there. One exchange at a time; a second concurrent `send` throws without
disturbing the first. Different clients are independent. One-shot and URL calls run on the context given (or the
one running them) and any number may run at once. A direct call blocks the calling thread and must not run on a
PACE context thread.

## Lifetime and ownership

- `basic_client` owns its stream. The client, its stream's context and the stream must stay alive and unmoved
  until `send` completes; destroying the client closes the connection.
- `send` owns its request. The returned `response` owns its data.
- The one-shot and URL calls own their arguments; the `io_context&` and a `tls::context&` are borrowed and must
  outlive the task. A `tls::stream` keeps its configuration alive once constructed, but the lazy task borrows it
  until then.
- A direct call owns everything it uses and leaves nothing running when it returns or throws.

## Limits and defaults

| Item | Default |
|:--|:--|
| `limits` | See the [HTTP/1 page](README.md#limits-and-defaults) |
| `client_options::close_policy` | `no_wait` |
| `client_options::close_timeout` | 100 ms (used by `graceful` only) |
| `target` of `async_get` (endpoint form) | `/` |
| `content_type` of `post` / `async_post` | `application/octet-stream` |
| Direct call timeout (`blocking_options::timeout`) | 30 s |
| Coroutine call deadline | None beyond the caller's |
| Default `https://` trust and ALPN | System roots, verification on, ALPN `http/1.1` |
| Exchanges per client at a time | 1 |

## See also

- [HTTP/1](README.md), [HTTP/1 server](server.md)
- [URL clients across HTTP/1, HTTP/2 and HTTP/3](../http/clients.md), [HTTP messages](../http/README.md)
- [TCP](../net/tcp.md), [TLS](../tls/README.md), [DNS](../dns/README.md), [`pace::run` and `blocking_options`](../runtime/run.md)
- Tutorial: [HTTP client](../../tutorial/02-http-client.md), [TLS and DNS](../../tutorial/10-tls-and-dns.md);
  guide: [HTTP recipes](../../guides/http-recipes.md)
- [RFC 9112, message body length](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3)
