# `pace::http`

Include `<pace/http.hpp>`. PACE provides a server layer for HTTP/1.0 and HTTP/1.1
over [`tcp::stream`](tcp.md), with owned request/response values and sequential
request handling within each persistent connection. It is not an HTTP client API.

## Message types

```cpp
struct header_field {
    std::string name;
    std::string value;
};
struct request {
    std::string method;
    std::string target;
    std::string version;
    std::vector<header_field> headers{};
    std::string body;
    bool keep_alive = true;
    std::string_view header(std::string_view name) const noexcept;
};
struct response {
    unsigned status = 200;
    std::string body;
    std::string content_type = "text/plain; charset=utf-8";
    std::vector<header_field> headers{};
    bool close = false;
};
```

All strings and vectors in these types own their contents. Parsed `method` and
`target` retain wire spelling; `version` is `HTTP/1.0` or `HTTP/1.1`. No URL decoding,
query parsing, router, cookie parser, JSON conversion or authentication is implied.
`headers` preserves field order and spelling while trimming surrounding spaces
and tabs from field values. `body` contains exactly the Content-Length payload,
including any binary bytes. With no Content-Length the parsed body is empty.

`request::header(name)` performs ASCII case-insensitive matching and returns the
**first** matching value, or an empty view. Missing and present-but-empty cannot be
distinguished with this accessor alone; inspect `headers` if that matters. The
view refers to request-owned storage. Copy it before destroying or modifying the
request or its header collection.

`keep_alive` reflects the request version and Connection tokens. HTTP/1.1 defaults
to persistence; HTTP/1.0 requires `keep-alive`. `close` takes precedence over
`keep-alive`. A handler's `response.close = true` requests closure after that
response. The request-count limit can also force closure.

Final response status must be in `[200, 599]`. PACE serializes a status line using
`HTTP/1.1`, including responses to HTTP/1.0 requests. Common codes get standard
reason phrases; other accepted codes use `Response`. There is no informational
response interface. The body is an owned in-memory string with no configured
response-size limit.

PACE generates `Content-Length`, `Connection` and, when nonempty, `Content-Type`.
Set `content_type = ""` to omit that field. **Do not add Content-Length,
Transfer-Encoding, Connection or Content-Type to `response.headers`**; these are
rejected case-insensitively. Custom names must be HTTP tokens and values must not
contain disallowed control characters or CR/LF. Invalid response metadata becomes
a closing 500 response, preventing header injection.

HEAD responses suppress the wire body while retaining the corresponding normal
body length. Status 204 and 304 suppress both body and Content-Length. Status 205
suppresses the body and sends Content-Length 0. The same HEAD suppression applies
to errors after the parser has recognized a HEAD method.

## Limits

```cpp
struct limits {
    std::size_t max_header_bytes = 16 * 1024;
    std::size_t max_body_bytes = 1024 * 1024;
    std::size_t max_requests_per_connection = 1000;
};
```

`max_header_bytes` includes the request line, headers and final CRLF delimiter.
`max_body_bytes` bounds the declared Content-Length and buffered request body.
`max_requests_per_connection` bounds the sequential request/response count; the
last response carries `Connection: close`. There is no aggregate connection,
memory, rate or response-body bound. These are byte/count limits, not timeouts.

Zero does not mean unlimited. A zero header allowance rejects parsing, zero body
allowance permits only empty bodies, and zero request count closes without serving
a request. Bounds are copied into serving tasks. Configure useful positive limits
for your protocol instead of relying on zero-valued special modes.

## Handler and serving functions

```cpp
using handler = std::function<task<response>(request)>;

task<void> serve_connection(tcp::stream stream, handler handle, limits bounds = {});
task<void> serve(tcp::listener& listener, handler handle, limits bounds = {});
```

A handler takes an **owned request value** and returns a lazy response task. PACE
awaits that task on the connection's context. Use a named coroutine function, for
example `pace::task<pace::http::response> route(pace::http::request request)`.
The callable must be storable in `std::function`; shared state reachable through
it remains the application's responsibility. Copies may be invoked concurrently
on different contexts. Within one connection handlers are sequential; buffered
pipelined requests are processed in order after each preceding response.

`serve_connection` owns the moved stream and a handler copy. It enables TCP_NODELAY,
parses requests, awaits handlers, writes complete responses and releases the stream
when finished. Do not access the original moved stream. The context must outlive
the task. A final response uses the stream's default bounded graceful close
(100 ms and 64 KiB discard allowance). This can extend closure briefly while a
peer keeps sending; it is not a request deadline.

`serve` borrows its listener and owns a handler copy. It continually accepts and
spawns one independent root `serve_connection` task for each accepted stream.
The listener and its context must outlive the serving task and its connections.
There is no returned connection registry or join handle. Closing the listener on
its loop ends accepting; existing connections continue. Stopping the owning
context cancels both accept and connection waits.

`serve` returns normally when accept reports `operation_canceled` or
`bad_file_descriptor`; other accept errors propagate and can fail the root loop.
`serve_connection` contains per-connection `system_error` failures, including
disconnects, broken pipes and context cancellation, so they do not stop other
clients. Other unexpected failures, such as allocation failures outside handler
conversion, can still escape. These functions do not provide an error logging hook.

Handler exceptions normally become a closing 500 response. A handler's
`system_error(operation_canceled)` exits as cancellation instead of producing a
500. Invalid response status, content type or custom headers also produce a
closing 500. An empty `handler` therefore fails requests with 500; pass a valid
callable. Transport failure while writing an error response may prevent delivery.

## Parser behavior and supported protocol

| Condition | Result |
|---|---|
| Invalid request line, method token, target or header syntax | 400 and close |
| HTTP/1.1 missing Host; duplicate/empty/invalid Host | 400 and close |
| Duplicate, malformed or overflowing Content-Length | 400 and close |
| Transfer-Encoding plus Content-Length | 400 and close |
| Declared body exceeds `max_body_bytes` | 413 and close |
| Request head exceeds `max_header_bytes` | 431 and close |
| Unsupported HTTP version | 505 and close |
| Any Transfer-Encoding without Content-Length | 501 and close |
| CONNECT or Upgrade request | 501 and close |
| Any Expect header, including `100-continue` | 417 and close |
| EOF during an incomplete request | 400 when a response can still be sent |
| EOF before a new request starts | Normal connection completion |

Valid methods other than CONNECT reach the application; it decides routing and
405/404 responses. Host validation is syntactic and does not select a virtual host
or enforce allowed origins. HTTP/1.0 may omit Host. Obsolete folded headers and
whitespace in header names are rejected. Trailer/chunked parsing, TLS, HTTP/2,
WebSocket/upgrades, CONNECT tunnels, streaming bodies and request deadlines are
not implemented. The implementation does not automatically add Date or Server.

See [HTTP services](../tutorial/04-http.md) for handler, limit and curl examples.
