# Direct clients and HTTP name migration

[中文](direct-clients.zh-CN.md) · [Quick start](../tutorial/00-quick-start.md)

These APIs require **PACE 0.4.0 or newer**.
They intentionally change the HTTP free-function names. Server registrations such
as `app.get`, `router.get`, and the persistent client's `send` keep their names.

## Choose how the call runs

| Usage | Direct result on this thread | Task on an existing PACE context |
|---|---|---|
| HTTP GET | `http::get(url)` | `co_await http::async_get(url)` |
| HTTP POST | `http::post(url, body)` | `co_await http::async_post(url, body)` |
| Other HTTP methods | `http::fetch(url, request)` | `co_await http::async_fetch(url, request)` |
| Fixed-length TCP reply | `tcp::exchange(peer, request, reply_size)` | `co_await tcp::async_exchange(peer, request, reply_size)` |
| UDP request/reply | `udp::exchange(peer, request)` | `co_await udp::async_exchange(peer, request)` |
| Send one UDP packet | `udp::send(peer, message)` | `co_await socket.send(message, peer)` on an owned UDP socket |

Direct calls create a fresh context on the caller's thread, complete one exchange,
close their owned socket and return owned data. They do not create helper threads,
pool connections or share a global loop. Each call incurs setup/teardown; use
persistent async clients for many requests or concurrent workloads. They must not
run inside a PACE task or callback: a current context causes `logic_error` before
network I/O, even when the call would complete immediately. Other GUI/event loops
are not detected; the call still blocks their thread.

## Signatures

```cpp
// <pace/blocking.hpp>, also included by tcp.hpp, udp.hpp and http.hpp
struct pace::blocking_options {
    std::chrono::milliseconds timeout{30000};
    cancellation_token cancellation{};
    pace::deadline deadline{};
};

// <pace/http.hpp>, link pace_http1; signatures inside namespace pace::http
response get(std::string url, blocking_options wait = {},
             limits bounds = {}, client_options options = {});
response post(std::string url, std::string body,
              std::string content_type = "application/octet-stream",
              blocking_options wait = {}, limits bounds = {}, client_options options = {});
response fetch(std::string url, request outgoing, blocking_options wait = {},
               limits bounds = {}, client_options options = {});
// With PACE_HAS_TLS:
response get(const tls::context& trust, std::string url, blocking_options wait = {},
             limits bounds = {}, client_options options = {});
response fetch(const tls::context& trust, std::string url, request outgoing,
               blocking_options wait = {}, limits bounds = {}, client_options options = {});

// <pace/tcp.hpp>, link pace_net; inside namespace pace::tcp
std::string exchange(endpoint peer, std::string request, std::size_t reply_size,
                     blocking_options options = {}, std::size_t maximum = 1024 * 1024);
task<std::string> async_exchange(io_context&, endpoint, std::string request,
                                 std::size_t reply_size, std::size_t maximum = 1024 * 1024);
task<std::string> async_exchange(endpoint, std::string request,
                                 std::size_t reply_size, std::size_t maximum = 1024 * 1024);

// <pace/udp.hpp>, link pace_net; inside namespace pace::udp
packet exchange(endpoint peer, std::string request, blocking_options options = {},
                std::size_t capacity = 65536);
std::size_t send(endpoint peer, std::string message, blocking_options options = {});
task<packet> async_exchange(io_context&, endpoint, std::string request, std::size_t capacity = 65536);
task<packet> async_exchange(endpoint, std::string request, std::size_t capacity = 65536);
```

## Deadlines and errors

A direct call's positive `blocking_options::timeout` covers connection, optional
DNS/TLS, request/response and cleanup. Zero/negative values throw `invalid_argument`.
The effective budget is the earlier of the positive timeout and absolute
`deadline`; `cancellation` can cancel it from another thread. On expiry the local
operation scope cancels and drains pending work, and the caller receives
`system_error` comparing equal to `std::errc::timed_out`. No background operation
outlives the call, and subsequent direct calls use independent contexts.

This is cooperative cancellation, not preemption: synchronous trust loading,
allocation, CPU work and OS scheduling can extend wall time beyond the budget.
A completion observed after the budget also reports timeout. Exceptions otherwise
propagate unchanged; HTTP 4xx/5xx remain successful transport responses. Timeout
cannot undo bytes already sent or effects already performed by a peer. No retry
is automatic.

The `async_` forms preserve the underlying asynchronous contracts and **do not add
this total deadline**. They inherit the task token and deadline; use
[with_options or with_deadline](structured-concurrency.md) for a local boundary. Socket member methods remain asynchronous regardless of whether
the name includes `async_`.

## HTTP and verified HTTPS

URL handling, framing limits and close policy are shared with
[async URL clients](convenience.md#url-client). Numeric HTTP works without DNS/TLS;
hostnames require `dns=y`, HTTPS requires `tls=y`. Missing optional support reports
`not_supported`. HTTPS verifies the URL host using system trust roots by default.
Keep an explicit TLS context alive for the whole direct call:

```cpp
pace::tls::context trust;
trust.load_verify_file("ca.pem");
auto reply = pace::http::get(trust, "https://localhost:8443/health");
```

For POST with custom trust, set `request.method`, `request.body` and a Content-Type
header, then call `fetch(trust, url, request)`. HTTP still owns framing headers;
URL helpers supply Host and target. There is no redirect following, proxy,
decompression or connection reuse in a one-shot call. For persistent connections,
see [HTTP recipes](http-recipes.md).

## TCP framing

The endpoint is numeric IPv4/IPv6. `reply_size` is an exact framing contract, not a
maximum read count. A larger size than `maximum` throws `length_error` before
connecting. An early EOF throws `system_error(connection_reset)`. Zero reply bytes
are allowed; the request is still written. Strings preserve embedded zero bytes.

After the reply, graceful close half-closes the send direction and drains with the
existing 100 ms / 64 KiB close bounds, still inside the blocking total budget.
For persistent sessions, delimiter/length-prefix framing, a different close policy
or reusable buffers, use `tcp::connect`, `pace::write` and `pace::read_exact` on the
owned stream. See [protocol tuning](protocol-tuning.md).

## UDP filtering and capacity

`exchange` sends one datagram, then waits for one reply from the same numeric IP,
port, family and IPv6 scope. The address is canonicalized before comparison.
Unrelated senders are ignored without extending the deadline. This filtering is
not authentication, correlation by application request ID, or a delivery guarantee.
Services that reply from a different endpoint need the socket API. These helpers
are for unicast peers; broadcast/multicast need explicitly configured sockets.

`capacity` must be 1–65536 bytes, checked before sending; zero throws
`invalid_argument`, excess throws `length_error`. The peer port must be nonzero.
A matched truncated packet is returned with `truncated=true`, not silently retried.
A zero-byte packet is valid. `send` returns the count accepted by the local socket;
it does not wait for or prove delivery. Both helpers use a fresh socket per call.

## Migrate from 0.3.1

| Before | Now |
|---|---|
| `pace::run(http::get(url))` | `http::get(url)` |
| `pace::run(http::post(url, body))` | `http::post(url, body)` |
| `co_await http::get(url)` | `co_await http::async_get(url)` |
| `co_await http::post(url, body)` | `co_await http::async_post(url, body)` |
| `co_await http::fetch(url, request)` | `co_await http::async_fetch(url, request)` |
| `co_await http::get(io, peer, target)` | `co_await http::async_get(io, peer, target)` |
| `co_await http::get(io, trust, url)` | `co_await http::async_get(io, trust, url)` |

All former HTTP free-function `get`, `post`, `fetch` overloads are renamed with
`async_`, including explicit-context, numeric endpoint and custom TLS overloads.
Their argument order remains unchanged. New direct HTTP functions accept a URL;
no direct overload takes an `io_context&`. When moving an old synchronous
`pace::run(http::get(url, bounds, options))`, add the new timeout slot:
`http::get(url, {}, bounds, options)`.

HTTP/2 and HTTP/3 now follow the same naming: URL `get/post/fetch` are direct,
`async_get/async_post/async_fetch` are asynchronous; owned-transport `get` and
`exchange` become `async_get` and `async_exchange`. See [unified clients](http-clients.md).
TCP/QUIC connect and socket member names are unchanged.
