# UDP (`pace::udp`)

[简体中文](udp.zh.md) · [Networking](README.md)

## Overview

`pace::udp` provides nonblocking UDP sockets bound to an `io_context`, owning send and receive helpers, and
one-call request/reply helpers.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/net/udp.hpp>` (included by `<pace/net.hpp>`) | `pace_net` | none (always built) |

Endpoints are numeric IPv4 or IPv6 addresses; resolve host names with the [DNS module](../dns/README.md). UDP keeps
message boundaries but does not guarantee delivery, order or uniqueness. With `--modules=y` the names are exported
by `import pace.net;`. The checked-in example is `examples/tutorial_udp.cpp` (`xmake run tutorial_udp`).

## Synopsis

```cpp
#include <pace/net.hpp>
#include <pace/runtime.hpp>

#include <iostream>

pace::task<void> hello()
{
    auto receiver = pace::udp::bind({"127.0.0.1", 0});
    auto sender = pace::udp::open();
    co_await sender.send("Hello, UDP!", receiver.local_endpoint());

    auto packet = co_await receiver.receive();
    std::cout << packet.data << '\n';
    co_await receiver.send("reply", packet.sender);
}

int main()
{
    pace::run(hello);
}
```

## Types

### `endpoint`

```cpp
struct endpoint
{
    std::string address = "127.0.0.1";
    std::uint16_t port = 0;
    address_family family = address_family::unspecified;
    std::uint32_t scope_id = 0;
};
```

The same fields and rules as [`tcp::endpoint`](tcp.md#types): a numeric, unbracketed IPv4 or IPv6 address, the
port, an optional family that must match the address, and the IPv6 interface index in `scope_id` (nonzero only for
IPv6). Host names, brackets, NUL bytes, malformed addresses and `%zone` suffixes throw `std::invalid_argument`;
write `{"fe80::1", port, pace::address_family::ipv6, 3}` instead of `fe80::1%3`. `::1` is IPv6 loopback and `::`
the IPv6 wildcard. Sender and local endpoints report the concrete family and keep the scope separately from the
address. PACE does not resolve interface names.

### `bind_options`

```cpp
struct bind_options
{
    bool ipv6_only = true;
};
```

On an IPv6 endpoint, sets `IPV6_V6ONLY` before binding, whatever the OS default. `false` makes one socket bound to
`::` receive both families, for example `pace::udp::socket receiver(io, {"::", 0}, {.ipv6_only = false});`. It must
stay `true` on an IPv4 endpoint. On such a dual-stack socket, IPv4 senders appear as IPv4-mapped IPv6 endpoints
(`::ffff:127.0.0.1`) that can be passed straight back to `send_to`, and `send_to` maps a plain IPv4 destination to
that form. An IPv6-only socket rejects IPv4 destinations.

### `datagram`

```cpp
struct datagram
{
    std::size_t size = 0;
    endpoint sender;
    bool truncated = false;
};
```

The result of `receive_from`. `size` is the number of bytes copied into the buffer; `0` is a valid empty datagram,
not EOF. `sender` is the source address and port. `truncated` is `true` when the datagram was larger than the
buffer: its remainder has been discarded, and the next receive starts with the next datagram.

### `packet`

```cpp
struct packet
{
    std::string data;
    endpoint sender;
    bool truncated = false;
};
```

The result of `receive` and `exchange`, owning its bytes. An empty `data` is a valid empty datagram. `truncated` has
the same meaning as in `datagram`.

### `socket`

```cpp
class socket
{
public:
    explicit socket(io_context& context);
    socket(io_context& context, address_family family);
    socket(io_context& context, endpoint local, bind_options options = {});
    ~socket();
    socket(socket&& other) noexcept;
    socket& operator=(socket&& other) noexcept;
    socket(const socket&) = delete;
    socket& operator=(const socket&) = delete;

    void bind(endpoint local, bind_options options = {});
    endpoint local_endpoint() const;
    void set_broadcast(bool enabled = true);
    task<std::size_t> send_to(std::span<const std::byte> bytes, endpoint destination);
    task<datagram> receive_from(std::span<std::byte> buffer);
    task<std::size_t> send(std::string bytes, endpoint destination);
    task<packet> receive(std::size_t capacity = 65536);
    void close() noexcept;

    bool is_open() const noexcept;
    native_socket_type native_handle() const noexcept;
    io_context& context() const noexcept;
};
```

A move-only UDP socket owned by one `io_context`.

#### Construction and family

| Constructor | Effect |
|:--|:--|
| `socket(io)` | Unbound socket. Starts with an IPv4 handle; the first `bind` or `send_to` with an IPv6 address replaces it with an IPv6 one |
| `socket(io, family)` | Unbound socket of an explicit family that keeps its handle and rejects addresses of the other family |
| `socket(io, local, options)` | Creates the socket and binds it to `local` |

The family of `socket(io)` is fixed by the first `bind`, `send_to`, `receive_from` or `set_broadcast` (the last two
fix IPv4); a later address of the other family throws `std::system_error` with `address_family_not_supported`. Use
the explicit-family constructor when you set native options on `native_handle()` before the first operation.
Construction can run before the context runs; OS failures throw `std::system_error`.

#### `bind`

```cpp
void bind(endpoint local, bind_options options = {});
```

Binds an idle socket to a local address. Port `0` picks a free port; read it back with `local_endpoint()`. Binding
while a send or receive is outstanding throws `std::logic_error`. Synchronous; may run before the context runs.

#### Sending

```cpp
task<std::size_t> send_to(std::span<const std::byte> bytes, endpoint destination);
task<std::size_t> send(std::string bytes, endpoint destination);
```

Sends one datagram and returns its size. `send_to` borrows the bytes until it completes; pass text as
`send_to(pace::as_bytes(text), destination)`. `send` owns its string. An empty payload sends an empty datagram. A payload larger than 65,507 bytes, or one the OS accepts only in part,
throws `std::system_error` with `std::errc::message_size`; the path MTU may make the useful limit much smaller.
Completion means the local socket accepted the datagram, not that it was delivered.

#### Receiving

```cpp
task<datagram> receive_from(std::span<std::byte> buffer);
task<packet> receive(std::size_t capacity = 65536);
```

Waits for one datagram. `receive_from` copies it into the borrowed `buffer`, which must not be empty
(`std::invalid_argument`). `receive` allocates `capacity` bytes, returns a `packet` holding only the received
bytes, and throws `std::invalid_argument` for `capacity == 0` and `std::length_error` for `capacity > 65536`, before
any I/O. A small capacity can truncate a datagram; check `truncated`. Use `receive_from` to reuse a buffer across
receives; a character buffer goes through `pace::as_writable_bytes`:

```cpp
std::array<char, 1500> buffer{};
for (;;)
{
    const auto received = co_await socket.receive_from(pace::as_writable_bytes(buffer));
    const std::string_view payload(buffer.data(), received.size);
    co_await socket.send_to(pace::as_bytes(payload), received.sender);
}
```

#### `set_broadcast`

```cpp
void set_broadcast(bool enabled = true);
```

Sets or clears `SO_BROADCAST`. IPv4 only: on an IPv6 socket it throws `std::system_error` with
`operation_not_supported`, since IPv6 has no broadcast. There is no multicast membership API.

#### `close` and accessors

`close()` cancels pending waits and closes the handle; it is idempotent and also run by the destructor. Suspended
operations then finish with `std::system_error`, so keep the object alive until they have unwound.
`local_endpoint()` returns the bound address and port and throws `std::system_error` on a closed socket.
`is_open()` reports whether the socket owns a handle, `native_handle()` returns the borrowed native socket (do not
close it or change its blocking mode) and `context()` the owning context.

## Functions

### `open` and `bind`

```cpp
socket open(io_context& io);
socket open();
socket bind(io_context& io, endpoint local, bind_options options = {});
socket bind(endpoint local, bind_options options = {});
```

`open` returns an unbound socket whose family is chosen on first use; `bind` returns a socket bound to `local`. Both
are synchronous. The overloads without a context call `pace::this_context()` at the call, so they must be called
from a task or callback on a running context; use the explicit-context overloads before `io.run()`.

### `exchange` and `async_exchange`

```cpp
task<packet> async_exchange(io_context& io, endpoint peer, std::string request, std::size_t capacity = 65536);
task<packet> async_exchange(endpoint peer, std::string request, std::size_t capacity = 65536);
packet exchange(endpoint peer, std::string request, blocking_options options = {}, std::size_t capacity = 65536);
```

Sends `request` as one datagram from a fresh socket of the peer's family, then returns the first reply that comes
from the same IP address, port, family and IPv6 scope (compared after canonicalizing the address). Datagrams from
other senders are dropped and do not extend any deadline. This filtering is not authentication, not correlation by
request ID, and not retransmission: a lost request or reply is only noticed through a deadline. Services that reply
from a different address or port, and broadcast or multicast, need a `socket`.

- `capacity` must be between 1 and 65536 bytes (`std::invalid_argument` for zero, `std::length_error` above), and
  `peer.port` must be nonzero (`std::invalid_argument`); both are checked before sending.
- A matching truncated reply is returned with `truncated = true`, not retried. An empty reply is valid.
- `async_exchange` runs on an existing context (the running one for the overload without `io`) and adds no
  deadline; it inherits the awaiting task's cancellation and deadline. Without one it waits for a reply forever.
- `exchange` blocks the calling thread, running the exchange on a fresh context with
  [`blocking_options`](../runtime/run.md). Its `timeout` (30 s by default) or `deadline` bounds the call; on expiry
  it throws `std::system_error` with `std::errc::timed_out`. A non-positive `timeout` throws `std::invalid_argument`,
  and a call from inside a PACE context throws `std::logic_error` before any I/O.

### `send`

```cpp
std::size_t send(endpoint peer, std::string message, blocking_options options = {});
```

Blocking: sends one datagram from a fresh socket on a fresh context and returns the byte count accepted by the local
socket. It does not wait for or prove delivery. `blocking_options` behaves as for `exchange`. Inside a task, use
`co_await socket.send(message, peer)` on an owned socket instead.

## Errors

| Condition | Exception |
|:--|:--|
| Malformed endpoint, host name, `%zone`, family or scope mismatch | `std::invalid_argument` |
| `ipv6_only = false` on an IPv4 endpoint | `std::invalid_argument` |
| Address of the other family after the family is fixed | `std::system_error` (`address_family_not_supported`) |
| Empty `receive_from` buffer, `receive`/`exchange` capacity 0, `exchange` port 0 | `std::invalid_argument` |
| `receive`/`exchange` capacity above 65536 | `std::length_error` |
| Payload above 65,507 bytes or sent only in part | `std::system_error` (`message_size`) |
| `set_broadcast` on IPv6 | `std::system_error` (`operation_not_supported`) |
| A second send or a second receive while one is outstanding; `bind` while busy | `std::logic_error` |
| Asynchronous operation run outside its owning context | `std::logic_error` |
| Operation on a closed socket | `std::system_error` (`bad_file_descriptor`) |
| Context stopped, socket closed during the wait, or task canceled | `std::system_error` (`operation_canceled`) |
| Deadline of the awaiting task, or `exchange`/`send` budget, expired | `std::system_error` (`timed_out`) |
| Other OS failure | `std::system_error` with the native code |

As with TCP, compare `error.code()` with `std::errc` conditions rather than numeric `errno` values.

## Threads and concurrency

- Asynchronous operations must run on the owning context; `bind`, `set_broadcast`, `local_endpoint` and `close` may
  also run before the context runs or after it has stopped. To close a socket from another thread, `post` the call
  to its context.
- One `send_to`/`send` and one `receive_from`/`receive` may be outstanding at the same time. A second operation in
  the same direction throws `std::logic_error` immediately.

## Lifetime and ownership

- The `io_context` must outlive every socket created on it.
- Keep the socket alive and at the same address while any operation is suspended, and keep `send_to` and
  `receive_from` buffers (including the storage behind a `pace::as_bytes` or `pace::as_writable_bytes` view) valid
  until their operation completes. `send`, `receive` and the free functions own their data.
- Move a socket only while it is idle. Move assignment closes the destination's previous socket first.

## Limits and defaults

| Item | Default or bound |
|:--|:--|
| `endpoint` defaults | `"127.0.0.1"`, port `0`, `unspecified`, scope `0` |
| `bind_options::ipv6_only` | `true` |
| Largest payload `send_to` accepts | 65,507 bytes |
| `receive` / `exchange` `capacity` | default 65536, allowed 1–65536 |
| `exchange` / `send` timeout (`blocking_options::timeout`) | 30 s |
| `async_exchange` timeout | none; use a deadline |

## See also

- [Networking overview](README.md), [TCP](tcp.md)
- [DNS](../dns/README.md) for host names, [QUIC](../quic/README.md) for reliable streams over UDP
- [Running a program](../runtime/run.md), [structured concurrency](../runtime/structured-concurrency.md)
- Tutorial: [UDP](../../tutorial/06-udp.md)
