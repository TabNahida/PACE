# Networking (`pace/net.hpp`)

[简体中文](README.zh.md) · [Reference](../README.md)

## Overview

The net module provides nonblocking TCP and UDP sockets over the PACE reactor, plus generic helpers that work on
any PACE byte stream (TCP, TLS and QUIC streams).

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/net.hpp>` | `pace_net` | none (always built) |

`<pace/net.hpp>` includes every header of the module:

| Header | Contents | Page |
|:--|:--|:--|
| `<pace/net/ip.hpp>` | `pace::address_family` | this page |
| `<pace/net/stream_io.hpp>` | `pace::write`, `pace::read_exact` for byte streams | this page |
| `<pace/net/tcp.hpp>` | `pace::tcp`: `endpoint`, `stream`, `listener`, `connect`, `listen`, `exchange` | [TCP](tcp.md) |
| `<pace/net/udp.hpp>` | `pace::udp`: `endpoint`, `socket`, `datagram`, `packet`, `open`, `bind`, `exchange`, `send` | [UDP](udp.md) |
| `<pace/net/aliases.hpp>` | Deprecated aliases (`tcp_stream`, …) | this page |

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
- Errors are reported as exceptions, mostly `std::system_error`; see each page's Errors section and
  [error handling](../errors.md).

## Synopsis

```cpp
#include <pace/net.hpp>
#include <pace/runtime.hpp>

#include <iostream>

pace::task<void> demo(pace::io_context& io)
{
    auto listener = pace::tcp::listen(io, {"127.0.0.1", 0});
    auto client = co_await pace::tcp::connect(io, listener.local_endpoint());
    auto server = co_await listener.accept();

    co_await pace::write(client, "ping");
    auto received = co_await pace::read_exact(server, 4);
    std::cout << received << '\n';
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

### Deprecated aliases

```cpp
// <pace/net/aliases.hpp>
namespace pace
{
using tcp_stream = tcp::stream;
using tcp_listener = tcp::listener;
using ipv4_endpoint = tcp::endpoint;
using listen_options = tcp::listen_options;
}
```

These names are deprecated and will be removed. Use the `pace::tcp` names in new code. `ipv4_endpoint`
is the full `tcp::endpoint` and accepts IPv6 too.

## Functions

The helpers in `<pace/net/stream_io.hpp>` work on any type with the PACE byte-stream members
`read_some(std::span<char>) -> task<std::size_t>` and `write_all(std::string_view) -> task<void>`:
[`tcp::stream`](tcp.md), [`tls::stream`](../tls/README.md) and [`quic::stream`](../quic/README.md).

### `write`

```cpp
template <class Stream>
task<void> write(Stream& stream, std::string bytes);
```

Writes every byte of `bytes` with `stream.write_all`. The task owns the string, so a temporary or a string that
goes out of scope before the task runs is safe: an lvalue argument is copied, an rvalue is moved. The string may
hold binary data, including zero bytes. Completion means the bytes were handed to the transport, not that the peer
received or processed them. Exceptions from `write_all` propagate.

### `read_exact`

```cpp
template <class Stream>
task<void> read_exact(Stream& stream, std::span<char> bytes);
template <class Stream>
task<void> read_exact(Stream& stream, std::span<std::byte> bytes);
template <class Stream>
task<std::string> read_exact(Stream& stream, std::size_t size, std::size_t maximum = std::size_t{1024} * 1024);
```

Reads exactly the requested number of bytes by calling `read_some` until the buffer is full. It never reads past
the requested length, so the bytes of the next message stay in the stream. An empty request completes without
reading.

- The span overloads fill the caller's buffer, which must stay valid until the task completes. If the operation
  fails, a prefix of the buffer may already have been overwritten.
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

## Threads and concurrency

The helpers run on the stream's owning context and inherit its concurrency rules. `read_exact` is one read
operation for its whole duration: start no other read on the same stream until it completes. On a `tls::stream`,
start no other operation at all, since TLS allows one operation at a time. `write` is one write operation.

## Lifetime and ownership

The stream is borrowed: keep it alive, at the same address and on its owning context until the task completes.
`write` owns its bytes; the span overloads of `read_exact` borrow their buffer.

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
