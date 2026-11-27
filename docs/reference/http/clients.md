# Unified HTTP clients

[中文](http-clients.zh-CN.md) · [Deadlines and concurrency](structured-concurrency.md)

These APIs require PACE 0.4.0 or newer. `pace::http1` is an alias of
`pace::http`; HTTP/1, HTTP/2 and HTTP/3 share `http::request` and `http::response`.

| Operation | Direct call | Coroutine call |
|---|---|---|
| GET | `get(url, wait={}, limits={})` | `co_await async_get(url, limits={})` |
| POST | `post(url, body, content_type, wait={}, limits={})` | `co_await async_post(url, body, content_type, limits={})` |
| Other methods | `fetch(url, request, wait={}, limits={})` | `co_await async_fetch(url, request, limits={})` |

Use these names in `pace::http`, `pace::http1`, `pace::http2` or `pace::http3`.
Content-Type defaults to `application/octet-stream`. Async URL forms also accept
`io_context&` as the first argument. HTTP/1 additionally accepts its existing
`client_options` after limits; limits are protocol-specific types.

Direct calls block the caller, own a fresh context and connection, return owned
responses, and reject use inside a running PACE context. `blocking_options` has
`timeout` (positive, default 30 seconds), `cancellation` and absolute `deadline`.
The earliest deadline wins. Async calls inherit their task's cancellation and
shared deadline; use `with_deadline`/`with_options` to bound a request or group.
There is no default total deadline on async requests.

| Version | URL and transport | Build target / options |
|---|---|---|
| HTTP/1 | `http://` TCP; verified `https://` with TLS | `pace_http1`; `http1=y`, optional `tls=y` |
| HTTP/2 | `http://` prior knowledge; verified `https://` with ALPN `h2` | `pace_http2`; `http2=y`, optional `tls=y` |
| HTTP/3 | verified `https://` with QUIC ALPN `h3`; rejects `http://` | `pace_http3`; `http3=y` (POSIX) |

Numeric IPv4/IPv6 URLs work without DNS. Hostnames require `dns=y`; missing
optional support reports `not_supported`. HTTP/1 and HTTP/2 try resolved TCP
addresses in sequence when connection establishment fails. HTTP/3 uses the first
resolved address. No protocol downgrades or request retries are automatic.

System trust is used by default. For a private CA, use `get(trust, url, ...)` or
`fetch(trust, url, request, ...)`; async forms are
`async_get(io, trust, url, ...)` / `async_fetch(io, trust, url, request, ...)`.
Trust is `tls::context` for HTTP/1 and HTTP/2, `quic::context` for HTTP/3. Configure
ALPN `h2` / `h3` for explicit contexts and retain them through completion.
For custom-trust POST use a request with method/body/Content-Type and `fetch`.

URL clients own authority and target; Host and pseudo-header overrides are
rejected. Responses are buffered within limits; 4xx/5xx are returned normally.
There is no redirect following, proxy, decompression or pooling. Repeated one-shot
calls open separate connections. To multiplex HTTP/2 or HTTP/3 on one connection,
use `async_exchange(owned_transport, authority, vector<request>, limits={})`.
Results preserve input order; any incomplete/reset response fails the batch.

## Migration and concurrency

Old transport-based `http2::get` / `http3::get` are `async_get`; old `exchange`
becomes `async_exchange`, with arguments unchanged. The unprefixed URL calls are
synchronous. [HTTP/1 migration](direct-clients.md#migrate-from-031) covers its
previous renames. TCP/QUIC `connect`, socket methods and server route registration
names are unchanged.

One batch owns one connection driver. Handlers can interleave on its context;
nghttp2/nghttp3 and the transport are never driven concurrently from multiple
threads. `serve_connection` cancels and joins handlers before it exits. `serve`
owns accepted tasks and cancels/joins them when acceptance ends. Closing a
listener therefore ends its owned connection scope. For a custom graceful drain
policy, own the accept loop and connection children in `with_task_group`.
See the [complete concurrency matrix](structured-concurrency.md#protocol-concurrency-and-server-lifetime).
