# Networking (`pace/net.hpp`)

[简体中文](README.zh.md) · [Reference](../README.md)

## Overview

The net module provides nonblocking TCP and UDP sockets over the PACE reactor, the `pace::byte_stream` concept that
TCP, TLS and QUIC streams share, generic helpers for any byte stream, and zero-copy views between text and bytes.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/net.hpp>` | `pace_net` | none (always built) |

`<pace/net.hpp>` includes every header of the module:

| Header | Contents | Page |
|:--|:--|:--|
| `<pace/net/ip.hpp>` | `pace::address_family` | this page |
| `<pace/net/buffer.hpp>` | `pace::as_bytes`, `pace::as_writable_bytes`, `pace::as_text` | this page |
| `<pace/net/stream_io.hpp>` | `pace::byte_stream`; `pace::write`, `pace::read_exact` for byte streams | this page |
| `<pace/net/tcp.hpp>` | `pace::tcp`: `endpoint`, `stream`, `listener`, `connect`, `listen`, `exchange` | [TCP](tcp.md) |
| `<pace/net/udp.hpp>` | `pace::udp`: `endpoint`, `socket`, `datagram`, `packet`, `open`, `bind`, `exchange`, `send` | [UDP](udp.md) |

`pace_net` depends on `pace_runtime` and on nothing outside the C++ standard library and the operating system's
socket API. With `--modules=y`, `import pace.net;` exports the names on these pages; also `import pace.runtime;`
for `io_context`, `task` and `run`, or `import pace;` for everything.

Rules shared by every page of the module:

- Addresses are numeric IPv4 or IPv6 strings. Nothing in this module resolves host names; use the
  [DNS module](../dns/README.md) first.
- Every socket belongs to one `io_context`, the one passed at construction (or the running one for the overloads
  without an `io_context&`). Its asynchronous operations run on that context's thread.
- Operations return lazy [`task`](../runtime/task.md)s: nothing happens until the task is awaited or spawned.
- Sockets are move-only. Move them only while no operation is outstanding.
- Streams and sockets read and write `std::byte`: `read_some(std::span<std::byte>)` and
  `write_all(std::span<const std::byte>)`, and the same span types for UDP datagrams. Text crosses that boundary
  through the views of [`<pace/net/buffer.hpp>`](#as_bytes-as_writable_bytes-and-as_text), without a copy:
  `write_all(pace::as_bytes("text"))`, `read_some(pace::as_writable_bytes(buffer))`.
- TCP and UDP hot paths accept `pace::as_result` for error-code reporting. The default overloads throw exceptions.
  See [error handling](../errors.md#result-returning-io).

## Client options

TCP, UDP and TLS client helpers take `pace::client_options` from `<pace/net/client_options.hpp>`. For example, `udp::async_exchange(peer, request, {.context = &io, .timeout = 2s, .max_reply_size = 512})`.

- Coroutines select the running context when execution starts. An explicit `context` must match it. Blocking helpers own a fresh context and reject a non-null `context` with `std::invalid_argument`.
- A supplied `timeout` must be positive. Async calls inherit their parent's deadline by default. Blocking calls default to 30 seconds. The earliest relative, absolute or inherited deadline applies. The `cancellation` token covers the whole operation.
- `max_reply_size` bounds the reply allocation. TCP/TLS default to 1 MiB and UDP to 65536 bytes. `tls` points to a borrowed TLS configuration. `transport` supplies native socket settings and the move-only pre-connect hook for TCP/TLS. Pass named options with `std::move(options)`.

## Synopsis

```cpp
#include <pace/net.hpp>
#include <pace/runtime.hpp>

#include <array>
#include <iostream>
#include <string_view>

pace::task<void> demo(pace::io_context& io)
{
    auto listener = pace::tcp::listen(io, {"127.0.0.1", 0});
    auto client = co_await pace::tcp::connect(listener.local_endpoint(), {.context = &io});
    auto server = co_await listener.accept();

    co_await pace::write(client, "ping"); // owns its string
    std::cout << co_await pace::read_exact(server, 4) << '\n';

    co_await server.write_all(pace::as_bytes("pong")); // borrows the bytes
    std::array<char, 64> buffer{};
    const auto count = co_await client.read_some(pace::as_writable_bytes(buffer));
    std::cout << std::string_view(buffer.data(), count) << '\n';
}

int main()
{
    pace::run(demo);
}
```

## Types

### `address_family`

```cpp
// <pace/net/ip.hpp>
namespace pace
{
enum class address_family
{
    unspecified,
    ipv4,
    ipv6
};
}
```

The address family of a TCP or UDP endpoint or socket.

| Value | As input (endpoint, socket constructor) | As output (endpoint inspection) |
|:--|:--|:--|
| `unspecified` | Infer the family from the numeric address; a socket picks it on first use | Never returned |
| `ipv4` | The address must be IPv4 | IPv4 socket address |
| `ipv6` | The address must be IPv6 (IPv4-mapped addresses such as `::ffff:127.0.0.1` count as IPv6) | IPv6 socket address |

An explicit family that does not match the address throws `std::invalid_argument`. `local_endpoint()`,
`remote_endpoint()` and received datagram senders always report the concrete family of the native socket address.

### `byte_stream`

```cpp
// <pace/net/stream_io.hpp>
namespace pace
{
template <class Stream>
concept byte_stream =
    requires(Stream& stream, const Stream& owner, std::span<std::byte> input, std::span<const std::byte> output) {
        { stream.read_some(input) } -> std::same_as<task<std::size_t>>;
        { stream.write_all(output) } -> std::same_as<task<void>>;
        { stream.shutdown_write() } -> std::same_as<task<void>>;
        { stream.close() } noexcept;
        { owner.context() } noexcept -> std::same_as<io_context&>;
    };
}
```

A connected, ordered, bidirectional byte stream on one `io_context`. [`tcp::stream`](tcp.md#stream),
[`tls::stream`](../tls/README.md#stream) and [`quic::stream`](../quic/README.md#stream) satisfy it, and so may
application types. [`pace::write`](#write), [`pace::read_exact`](#read_exact) and the generic
[`tls::stream` constructor](../tls/README.md#custom-transports) accept any `byte_stream`. A `udp::socket` is not
one: it carries datagrams, not a stream.

The concept checks the signatures. A type that models it also keeps this contract:

| Member | Contract |
|:--|:--|
| `read_some(buffer)` | Completes with 1 to `buffer.size()` bytes, or with 0 only at end of stream (and for an empty buffer) |
| `write_all(bytes)` | Completes once every byte was accepted by the transport, not when the peer received it |
| `shutdown_write()` | Ends this direction: the peer reads end of stream, while this side can still read |
| `close()` | Releases the stream at once; suspended operations resume with `std::errc::operation_canceled` |
| `context()` | The `io_context` that runs every operation of the stream |

One read and one write may be outstanding at a time, unless the type documents a stricter limit: a `tls::stream`
allows one operation in total. Buffers are borrowed until the operation completes.

| Type | `shutdown_write()` | `close()` |
|:--|:--|:--|
| `tcp::stream` | Sends FIN | Closes the socket |
| `tls::stream` | Sends `close_notify`; reading continues until the peer's | Closes the transport without `close_notify` |
| `quic::stream` | Queues FIN, like `finish_write()` | Resets both directions, like `reset()` |

An application type satisfies the concept by forwarding to a stream it owns, for example to count the bytes it
receives:

```cpp
#include <pace/net.hpp>
#include <pace/runtime.hpp>

#include <cstddef>
#include <span>
#include <utility>

class counting_stream
{
public:
    explicit counting_stream(pace::tcp::stream inner)
        : inner_(std::move(inner))
    {
    }

    pace::task<std::size_t> read_some(std::span<std::byte> buffer)
    {
        const auto count = co_await inner_.read_some(buffer);
        received_ += count;
        co_return count;
    }

    pace::task<void> write_all(std::span<const std::byte> bytes)
    {
        return inner_.write_all(bytes);
    }

    pace::task<void> shutdown_write()
    {
        return inner_.shutdown_write();
    }

    void close() noexcept
    {
        inner_.close();
    }

    pace::io_context& context() const noexcept
    {
        return inner_.context();
    }

    std::size_t received() const noexcept
    {
        return received_;
    }

private:
    pace::tcp::stream inner_;
    std::size_t received_ = 0;
};

static_assert(pace::byte_stream<counting_stream>);
```

## Functions

### `as_bytes`, `as_writable_bytes` and `as_text`

```cpp
// <pace/net/buffer.hpp>
namespace pace
{
std::span<const std::byte> as_bytes(std::string_view text) noexcept;
std::span<std::byte> as_writable_bytes(std::span<char> characters) noexcept;
std::span<std::byte> as_writable_bytes(std::string& text) noexcept;
std::string_view as_text(std::span<const std::byte> bytes) noexcept;
}
```

Views between text and the bytes that streams read and write. None of them copies or allocates: the result points
at the argument's storage and is valid only as long as that storage is.

| Function | Result |
|:--|:--|
| `as_bytes(text)` | The characters of `text` as bytes, for `write_all` and `send_to`; takes a string literal, `std::string` or `std::string_view` |
| `as_writable_bytes(characters)` | A character buffer (`std::array<char, N>`, `std::vector<char>`, a `char` array) as writable bytes, for `read_some` and `receive_from` |
| `as_writable_bytes(text)` | The characters of an already-sized `std::string` as writable bytes, to read into it; the size does not change |
| `as_text(bytes)` | Bytes as a `std::string_view`, for parsing what was read |

```cpp
co_await stream.write_all(pace::as_bytes("GET / HTTP/1.0\r\n\r\n"));

std::string reply(512, '\0');
reply.resize(co_await stream.read_some(pace::as_writable_bytes(reply)));
```

A temporary `std::string` passed to `as_bytes` lives until the end of the full expression, so
`co_await stream.write_all(pace::as_bytes(prefix + body))` is safe; a view stored in a variable must not outlive its
string. Unlike `std::as_bytes`, `pace::as_bytes` takes text, not a span. Buffers that already hold `std::byte`, such
as `std::array<std::byte, N>`, are passed directly.

### `write`

```cpp
template <byte_stream Stream>
task<void> write(Stream& stream, std::string bytes);
```

Writes every byte of `bytes` with `stream.write_all`. The task owns the string, so a temporary or a string that
goes out of scope before the task runs is safe: an lvalue argument is copied, an rvalue is moved. The string may
hold binary data, including zero bytes. Completion means the bytes were handed to the transport, not that the peer
received or processed them. Exceptions from `write_all` propagate.

### `read_exact`

```cpp
template <byte_stream Stream>
task<void> read_exact(Stream& stream, std::span<std::byte> bytes);
template <byte_stream Stream>
task<std::string> read_exact(Stream& stream, std::size_t size, std::size_t maximum = std::size_t{1024} * 1024);
```

Reads exactly the requested number of bytes by calling `read_some` until the buffer is full. It never reads past
the requested length, so the bytes of the next message stay in the stream. An empty request completes without
reading.

- The span overload fills the caller's buffer, which must stay valid until the task completes. If the operation
  fails, a prefix of the buffer may already have been overwritten. Pass a character buffer as
  `pace::as_writable_bytes(buffer)`.
- The `size` overload returns an owned string of exactly `size` bytes. It throws `std::length_error` when
  `size > maximum`, before allocating or reading. `maximum` bounds the allocation, not the stream; raise it for
  larger messages.

`read_exact` adds no timeout and no framing of its own: the caller decides how many bytes the next message has
(for example from a length prefix). Use a [deadline](../runtime/structured-concurrency.md) to bound the wait.

## Errors

| Condition | Exception |
|:--|:--|
| EOF before the requested bytes arrived (`read_exact`) | `std::system_error` with `std::errc::connection_reset` |
| `size > maximum` (`read_exact`) | `std::length_error`, before any I/O |
| Transport error, cancellation, closed stream | Propagated unchanged from `read_some` / `write_all` |

`as_bytes`, `as_writable_bytes` and `as_text` are `noexcept` and cannot fail.

## Threads and concurrency

The helpers run on the stream's owning context and inherit its concurrency rules. `read_exact` is one read
operation for its whole duration: start no other read on the same stream until it completes. On a `tls::stream`,
start no other operation at all, since TLS allows one operation at a time. `write` is one write operation. The
views of `<pace/net/buffer.hpp>` do no I/O and may be used on any thread.

## Lifetime and ownership

The stream is borrowed: keep it alive, at the same address and on its owning context until the task completes.
`write` owns its bytes; the span overload of `read_exact` borrows its buffer. The views of `<pace/net/buffer.hpp>`
borrow the storage they point at: keep it alive and unchanged until every operation that uses the view has
completed.

## Limits and defaults

| Item | Value |
|:--|:--|
| `read_exact` `maximum` default | 1 MiB (`1024 * 1024` bytes) |

## See also

- [TCP](tcp.md), [UDP](udp.md)
- [TLS](../tls/README.md), [DNS](../dns/README.md), [QUIC](../quic/README.md)
- [Tasks](../runtime/task.md), [running a program](../runtime/run.md), [structured concurrency](../runtime/structured-concurrency.md)
- Tutorial: [TCP](../../tutorial/05-tcp.md), [UDP](../../tutorial/06-udp.md)
- Guide: [protocol tuning](../../guides/protocol-tuning.md)
- [Migrating to the byte interface](../../migration/0.6-to-1.0.md#byte-streams-have-one-interface-in-bytes)

## Bidirectional relay

`co_await pace::relay(a, b, options)` forwards both directions on the streams' common context. TCP streams, bidirectional QUIC streams and handshaken TLS streams can be paired in any combination. The relay owns the TLS session for the duration of the transfer. Custom transports must permit one concurrent read and write. EOF half-closes the opposite writer using TCP FIN, QUIC FIN or TLS `close_notify`, so the peer can still send a reply. Completion returns the byte counts `a_to_b` and `b_to_a`. An error, cancellation or timeout during the transfer closes both endpoints.

`relay_options::idle_timeout` bounds time without progress. `linger` bounds the reverse direction after the first EOF. Both default to zero, disabling the deadline. An optional fourth argument owns an observer whose `before_write(direction, size)` returns `task<void>` and can rate-limit writes. For two TCP streams on Linux, `.splice = true` requests zero-copy forwarding. An observer selects the copy path.
