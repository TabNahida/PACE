# HTTP/1 server

[简体中文](server.zh.md) · [HTTP/1](README.md)

## Overview

`serve` accepts connections on a listener and runs a handler for every request; `serve_connection` serves one
connection that is already open. Two handler forms exist: the buffered `handler`, and the `server_handler` with
`server_options`, which adds admission before the body is read, a body limit per request, receive and write
deadlines and responses streamed from a `body_source`.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/http1.hpp>` | `pace_http1` | `--http1=y` (default); `--tls=y` for the `tls::stream` and `tls::context` overloads |

Requests are buffered: the handler runs once the whole request, body and trailers included, has been received
within the limits. The message rules and the framing errors are on the [HTTP/1 page](README.md).

## Synopsis

```cpp
#include <pace/http1.hpp>

#include <chrono>
#include <string>

pace::task<pace::http::server_response> upload(pace::http::request request)
{
    co_return pace::http::text("received " + std::to_string(request.body.size()) + " bytes\n");
}

pace::task<void> run_server(pace::tcp::listener& listener)
{
    pace::http::server_options options;
    options.admit = [](const pace::http::request_head& head)
    {
        if (head.header("Authorization") != "Bearer example")
        {
            return pace::http::admission::reject(pace::http::text("unauthorized\n", 401));
        }
        // A larger body limit for one path, the default for the others.
        return head.target == "/upload" ? pace::http::admission::accept(16 * 1024 * 1024)
                                        : pace::http::admission::accept();
    };
    options.request_timeout = std::chrono::seconds(15);
    options.write_timeout = std::chrono::seconds(30);
    co_await pace::http::serve(listener, upload, options);
}
```

## Types

### `handler`, `server_handler`, `server_response`

```cpp
using handler = std::function<task<response>(request)>;  // from <pace/http/types.hpp>
using server_response = std::variant<response, streaming_response>;
using server_handler = std::function<task<server_response>(request)>;
```

A `handler` returns a buffered `response`; a [`router`](../http/router.md) or a lambda adapted with
`make_handler` is a `handler`. A `server_handler` may return either a buffered `response` or a
`streaming_response`. The `handler` overloads take `limits`; the `server_handler` overloads take
`server_options`.

### `server_options`

```cpp
struct server_options
{
    limits bounds{};
    std::function<admission(const request_head&)> admit{};
    std::chrono::milliseconds request_timeout{0};
    std::chrono::milliseconds write_timeout{0};
    std::size_t response_buffer_bytes = std::size_t{64} * 1024;
};
```

| Member | Meaning |
|:--|:--|
| `bounds` | Receive [limits](README.md#limits) |
| `admit` | Called for every request after its head has been validated and before its body is read. Empty means accept everything with `bounds.max_body_bytes` |
| `request_timeout` | Budget for receiving one request; zero disables it |
| `write_timeout` | Budget for writing one response; zero disables it |
| `response_buffer_bytes` | Largest span handed to `body_source::read`; must not be zero |

**Request deadline.** Starts before the request head is read, which includes waiting for the next request on a
keep-alive connection, and ends when the body and trailers are complete or admission rejects. It includes the
admission call. It is an absolute budget, not an idle timer: bytes trickling in do not extend it.

**Write deadline.** Starts when the response is ready, before its head is encoded, and covers every byte of the
response and every `body_source::read` call. Error replies and admission rejections use it too. It ends before
the graceful close, which keeps its own bound.

When a deadline expires, the connection is closed and its pending operations are cancelled. No 408 or other reply
is sent, and a response that was partly written stays incomplete. Neither deadline covers the handler, the TCP
accept or the TLS handshake: bound handler work yourself, for example with `with_deadline`. A synchronous
callback (`admit`, `body_source::read`) cannot be interrupted; expiry is checked after it returns. Timers are
cancelled when their phase ends, so a long keep-alive connection does not accumulate them.

### `request_head`

```cpp
struct request_head
{
    std::string_view method;
    std::string_view target;
    std::string_view version;
    std::span<const header_field> headers;
    std::optional<std::size_t> content_length;
    bool chunked = false;
    bool keep_alive = true;
    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
};
```

The validated head of a request, passed to `admit`. The request line, header fields and framing have passed the
checks on the [HTTP/1 page](README.md#errors); malformed input never reaches admission. `content_length` is the
declared length, empty for a chunked request or one without a body length. `header(name)` returns the first field
whose name matches ignoring ASCII case, or an empty view. Every view and the span are valid only during the
`admit` call; copy what you need to keep. No body bytes have been consumed yet, but the last socket read (at most
8 KiB) may already hold some body or pipelined bytes.

### `admission`

```cpp
struct admission
{
    std::optional<std::size_t> max_body_bytes;
    std::optional<response> rejection;

    static admission accept(std::optional<std::size_t> maximum = {});
    static admission reject(response result);
};
```

The decision returned by `admit`.

- `accept(n)` reads the body with a limit of `n` bytes for this request only. `n` may be larger or smaller than
  `bounds.max_body_bytes`. `accept()` keeps `bounds.max_body_bytes`; `accept(0)` allows only an empty body. A
  Content-Length above the limit is answered with 413 before the body is read; a chunked body is checked at each
  chunk-size line, before that chunk is read. The other limits are unchanged, and the limit does not carry over
  to the next request on the connection.
- `reject(result)` sends `result` at once, without reading the body and without running the handler, and closes
  the connection whatever `result.close` says. The graceful close that follows may discard the unread body.

### `body_source`

```cpp
class body_source
{
public:
    virtual ~body_source() = default;
    virtual std::size_t read(std::span<char> destination) = 0;
};
```

Produces the body of a `streaming_response` on demand. `read` fills at most `destination.size()` bytes and returns
how many it wrote. It must return a positive count no larger than the span. The span is never larger than
`response_buffer_bytes` or the bytes still due. `read` is called again only after the previous bytes have been
written, so the server never holds more than one buffer of the body in memory (the kernel and TLS keep their own
bounded buffers). It is not called once Content-Length bytes have been produced.

`read` runs synchronously on the connection's event loop and must return quickly: it suits generated data and
reads from a file that is already open, not blocking network or disk work. Disconnect, cancellation or write
deadline expiry stops further calls and destroys the source.

### `streaming_response`

```cpp
struct streaming_response
{
    response head;
    std::uint64_t content_length = 0;
    std::unique_ptr<body_source> source;
};
```

A response whose body comes from `source`, framed with `Content-Length: content_length` on HTTP/1.0 and HTTP/1.1.
`head` provides the status, content type and header fields; its `body` and `trailers` must be empty and its
`chunked` false. Length-less streaming and streamed trailers are not supported. `source` may be null when no byte
will be read: zero length, a `HEAD` request, or status 204, 205 or 304. For `HEAD` the Content-Length is still
announced. The source is destroyed on every path, including those where it is never read.

```cpp
#include <pace/http1.hpp>

#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

class file_body final : public pace::http::body_source
{
public:
    explicit file_body(const std::string& path)
        : file_(path, std::ios::binary | std::ios::ate)
    {
        if (!file_)
        {
            throw std::runtime_error("cannot open snapshot");
        }
        const auto end = file_.tellg();
        if (end < 0)
        {
            throw std::runtime_error("cannot size snapshot");
        }
        size_ = static_cast<std::uint64_t>(end);
        file_.seekg(0);
    }

    std::uint64_t size() const noexcept
    {
        return size_;
    }

    std::size_t read(std::span<char> destination) override
    {
        file_.read(destination.data(), static_cast<std::streamsize>(destination.size()));
        if (file_.bad())
        {
            throw std::runtime_error("cannot read snapshot");
        }
        return static_cast<std::size_t>(file_.gcount());
    }

private:
    std::ifstream file_;
    std::uint64_t size_ = 0;
};

pace::task<pace::http::server_response> backup(pace::http::request)
{
    auto file = std::make_unique<file_body>("backup.tar");
    pace::http::streaming_response result;
    result.head.content_type = "application/octet-stream";
    result.content_length = file->size();
    result.source = std::move(file);
    co_return result;
}
```

The size is taken from the same open file the body is read from. Keeping the file open does not stop another
writer from changing it; if it shrinks, `read` returns zero early and the connection is closed.

## Functions

### `serve_connection`

```cpp
task<void> serve_connection(tcp::stream stream, handler handle, limits bounds = {});
task<void> serve_connection(tcp::stream stream, server_handler handle, server_options options = {});
// With PACE_HAS_TLS:
task<void> serve_connection(tls::stream stream, handler handle, limits bounds = {});
task<void> serve_connection(tls::stream stream, server_handler handle, server_options options = {});
```

Serves requests on one connection until it closes. The task owns the stream and the handler. It enables
`TCP_NODELAY`. A `tls::stream` must have completed its handshake, and its negotiated ALPN protocol must be
`http/1.1` or none; otherwise the connection is ended. Requests are read, handled and answered one at a time;
pipelined requests already received are kept and served in order. After a response that ends the connection, the
stream is closed gracefully (half-close, then discard input for at most 100 ms and 64 KiB).

Transport errors, TLS errors and cancellation end this connection only: the task completes normally. The
`server_options` overloads throw `std::invalid_argument` at start for negative timeouts or a zero
`response_buffer_bytes`.

### `serve`

```cpp
task<void> serve(tcp::listener& listener, handler handle, limits bounds = {});
task<void> serve(tcp::listener& listener, server_handler handle, server_options options = {});
task<void> serve(io_context& context, tcp::endpoint endpoint, handler handle, limits bounds = {});
// With PACE_HAS_TLS:
task<void> serve(tcp::listener& listener, const tls::context& configuration, handler handle, limits bounds = {});
task<void> serve(
    tcp::listener& listener,
    const tls::context& configuration,
    server_handler handle,
    server_options options = {}
);
```

Accepts connections in a loop and runs `serve_connection` for each one in its own task, with a copy of the
handler. All accepted connections belong to this call: when accepting stops, they are cancelled, and `serve`
returns only after every one of them has finished.

- The listener overloads borrow the listener, which must outlive the task. Accepting stops when the listener is
  closed or the task is cancelled (for example by stopping the context); `serve` then returns normally. Any other
  accept error is thrown.
- The TLS overloads wrap each accepted socket with `configuration` and run the handshake inside that connection's
  task, so a slow handshake does not delay other clients. Configure the certificate (and ALPN `http/1.1` if you
  offer ALPN) before calling. A failed handshake ends only that connection.
- The `io_context&, tcp::endpoint` overload creates and owns the listener. It binds when the task starts, not when
  it is created, and bind errors are thrown from the task. It exists for the buffered `handler` over TCP only; use
  a listener you create for immediate bind errors, an ephemeral port's number, TLS, `server_options` or closing
  the listener separately.

There is no connection limit, no connection registry and no built-in drain of in-flight requests: closing the
listener cancels the connections it accepted. For a custom shutdown policy, run your own accept loop and own the
connections in a `with_task_group` ([structured concurrency](../runtime/structured-concurrency.md)).

## Errors

| Situation | Result |
|:--|:--|
| Malformed or over-limit request | Status reply from the [framing table](README.md#errors), then close |
| Handler throws (including an empty `std::function`) | 500 `Internal Server Error\n`, then close |
| Handler throws `std::system_error` with `operation_canceled` | Connection ends without a reply |
| `admit` throws | 500, then close; `operation_canceled` ends the connection |
| Invalid response metadata (status outside 200–599, managed or invalid fields, trailers without `chunked`, streaming `head` with a body) | 500, then close, instead of the response |
| Streaming: `read` returns 0 early or too many bytes, or throws | Connection closed; no second response is written after a partial one |
| Request or write deadline expires | Connection closed, no reply |
| Transport or TLS error | Connection ends; `serve_connection` completes normally |
| Accept error other than cancellation or a closed listener | Thrown from `serve` |
| `server_options` invalid | `std::invalid_argument` from `serve` or `serve_connection` |
| Bind error (`serve(context, endpoint, …)`) | `std::system_error` from the task |

Errors from one connection never stop the others. There is no logging hook. A reply that was written is accepted
by the local socket, not necessarily received by the peer.

## Threads and concurrency

Everything runs on the listener's context. Requests on one connection are handled sequentially; connections are
independent tasks and interleave on that context. To use several threads, run a listener and a `serve` per worker
context (see `app::run_workers` on the [application page](app.md)); the handler copies then run at the same time on
different threads and must synchronize the state they share. `admit` and `body_source::read` are synchronous calls
on the event loop thread.

## Lifetime and ownership

- `serve_connection` owns its stream, handler and options.
- `serve` copies the handler into every connection. It borrows the listener and, for TLS, the `tls::context`;
  both, and the context that runs the task, must outlive it.
- A `streaming_response` owns its `body_source` until the body is written or the connection fails.
- Views in `request_head` live only during the `admit` call.

## Limits and defaults

| Item | Default |
|:--|:--|
| `limits` | See the [HTTP/1 page](README.md#limits-and-defaults) |
| `server_options::request_timeout` | 0 (disabled) |
| `server_options::write_timeout` | 0 (disabled) |
| `server_options::response_buffer_bytes` | 64 KiB |
| `server_options::admit` | Empty: every request accepted with `bounds.max_body_bytes` |
| Socket read size | 8 KiB |
| Graceful close after the last response | Up to 100 ms and 64 KiB of discarded input |
| Concurrent connections | Unbounded |

## See also

- [HTTP/1](README.md), [HTTP/1 client](client.md), [HTTP/1 application](app.md)
- [Routing](../http/router.md), [gateway services](../gateway/README.md)
- [TCP](../net/tcp.md), [TLS](../tls/README.md), [structured concurrency](../runtime/structured-concurrency.md)
- Tutorial: [HTTP server](../../tutorial/03-http-server.md), [graceful shutdown](../../tutorial/09-graceful-shutdown.md)
