# Application conveniences (0.3.0)

[Quick start](../tutorial/00-quick-start.md) · [中文入门](../tutorial/00-quick-start.zh-CN.md)

All existing explicit-context, listener, transport and protocol APIs remain
available. These helpers use the existing reactor, without extra worker threads.

## Entry and the current context

`pace::run` accepts `task<T>`, a factory taking `io_context&`, or a factory taking
no arguments. If both signatures are invocable, the context form wins. Factories
are owned until shutdown. A task passed directly must already own its data: do
not immediately invoke a temporary capturing coroutine lambda and pass its task.
Pass the lambda itself. See [run](run.md) for result, cancellation and lifetime rules.

Header `<pace/this_context.hpp>` exports `this_context()`, `sleep_for(duration)`
and `yield()`. They resolve the running context when called, throw `logic_error`
when none is running, and return the same awaiters as the explicit context API.
Keep an awaiter on its original context and do not retain it beyond that context.
These helpers add no coroutine allocation. `run.hpp` also exports them.

## TCP

Header `<pace/tcp.hpp>`, link `pace_net`:

```cpp
pace::task<pace::tcp::stream> pace::tcp::connect(pace::io_context&, pace::tcp::endpoint);
pace::task<pace::tcp::stream> pace::tcp::connect(pace::tcp::endpoint);
// Members of tcp::stream:
pace::task<void> read_exact(std::span<char>);
pace::task<void> read_exact(std::span<std::byte>);
pace::task<std::string> read_exact(std::size_t size, std::size_t maximum = 1024 * 1024);
```

Endpoints remain numeric IPv4/IPv6; there is no implicit DNS at this layer.
The context-free connect selects the context when its task executes. The returned
socket must remain inside that context's lifetime; do not return it from `run`.

`read_exact` loops until the requested size is filled, never consumes trailing
bytes, and returns immediately for size zero. Premature EOF throws
`system_error(connection_reset)`; cancellation/transport errors propagate. A
failed span read may have modified a prefix. No partial result is returned by the
string overload. It checks `maximum` before allocation or I/O and throws
`length_error` on excess. Keep the stream and borrowed buffer alive and unmoved;
no other read may overlap the entire operation. There is no additional timeout.

## Response helpers and handlers

`http::text(body, status=200)`, `html` and `json` return owned `response` objects
with text/plain UTF-8, text/html UTF-8 and application/json respectively. They do
not escape content or serialize/validate JSON. Headers and other response fields
remain directly editable.

`make_handler`, `router` and `app` accept either a callable taking `request` or a
callable with no parameters, returning `response` or `task<response>`. The request
form takes precedence if both work. The existing [owned callable contract](http-router.md)
applies to both, including move-only captures. Mutable captures shared across
workers still require synchronization. Synchronous callbacks run on the reactor.

## HTTP application

Header `<pace/http.hpp>`, link `pace_http1`:

```cpp
pace::http::app app;
app.get("/", [] { return pace::http::text("hello"); });
// Also: post(path, handler), route(method, path, handler), all return app&.
app.run(8080); // loopback; run(endpoint, limits={}) selects another address
```

`serve(io_context&, endpoint, limits={})` and `serve(tcp::listener&, limits={})`
return a task with a snapshot of the routes, captured at the serve call. They do
not borrow the app. The listener overload still borrows the listener; keep it alive
until serving finishes. The endpoint overload binds lazily and owns its listener.
`handler()` returns a router snapshot for HTTP/2, HTTP/3, TLS or custom adapters.

`run(port=8080, limits={})` and `run(endpoint, limits={})` block on a fresh context
on this thread and propagate bind/runtime errors. Nested runs reject. There is no
signal handler, automatic graceful drain, worker pool, middleware, parameterized
routing or URL decoding. For those control points use the existing APIs. Route
lookup remains linear; snapshots share the table until registration detaches it.

## URL client

`get(io, url, limits={}, client_options={})`, `post(io, url, body,
content_type="application/octet-stream", limits={}, client_options={})`, and
`fetch(io, url, request, limits={}, client_options={})` perform one HTTP/1 exchange.
Each also has an overload without `io`, selecting the running context lazily.
All strings and requests are owned by the returned task.

URL fetch replaces `request.target`, supplies Host and sets `keep_alive=false`.
A caller-supplied Host is rejected. Method, body, other headers and trailers stay
under caller control. URL GET/POST select their corresponding method. Responses
retain status codes such as 404/500; transport/framing failures throw.

Accepted URLs have an absolute http or https scheme (case-insensitive), an ASCII
DNS host or numeric address, an optional decimal port 1–65535, and an optional
path/query. IPv6 must be bracketed. Default ports are 80/443; an empty path becomes
`/`; a query without a path becomes `/?...`; fragments are not sent. Userinfo,
backslashes, control/space/non-ASCII characters, invalid DNS labels, percent-encoded
hosts and zone identifiers are rejected with `invalid_argument`. Encode non-ASCII
path/query data yourself; existing escapes are preserved rather than decoded.
This is an HTTP client URL subset, not a general URI or browser URL parser.

Numeric HTTP requires only the default HTTP/1 build. Hostnames require `dns=y`
(async c-ares); HTTPS requires `tls=y` (OpenSSL). Missing support throws
`system_error(not_supported)` without silently downgrading. DNS follows the
resolver's default deadline/retry configuration. Candidate addresses are tried
sequentially only until TCP connects, without Happy Eyeballs; an individual
connect uses the OS timeout. Once connected, TLS/HTTP failures never retry the
request. There is no total request/connect deadline, redirect following, proxy,
cookie jar, caching or decompression. Use lower-level APIs for those policies.

Default HTTPS creates a verified system-trust configuration with HTTP/1.1 ALPN.
The URL host determines certificate identity and SNI independently of the resolved
address. Custom trust uses `get(io, tls_configuration, url, ...)` or
`fetch(io, tls_configuration, url, request, ...)`; the configuration must outlive
the lazy task and the URL must use https. These preserve the existing TLS peer
verification and ALPN checks. No insecure option is provided.
