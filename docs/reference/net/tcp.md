# TCP (`pace::tcp`)

[简体中文](tcp.zh.md) · [Networking](README.md)

## Overview

`pace::tcp` provides nonblocking TCP client and server sockets bound to an `io_context`, and one-call
request/reply helpers.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/net/tcp.hpp>` (included by `<pace/net.hpp>`) | `pace_net` | none (always built) |

Endpoints are numeric IPv4 or IPv6 addresses; there is no implicit name lookup. `tcp::stream` is a
[`pace::byte_stream`](README.md#byte_stream): it reads and writes `std::byte` spans, so text goes through
[`pace::as_bytes` and `pace::as_writable_bytes`](README.md#as_bytes-as_writable_bytes-and-as_text), and the generic
[`pace::write` and `pace::read_exact`](README.md#functions) helpers work on it. With `--modules=y` the names are
exported by `import pace.net;`.

## Native sockets and connection options

`tcp::stream::adopt(io, fd)` accepts an idle connected or unconnected TCP socket. `tcp::listener::adopt(io, fd)` accepts a listening socket. Success transfers exclusive ownership. On failure the caller owns the handle. Adoption sets nonblocking and non-inheritable flags. Pass a handle that is free of registrations on other loops and owned by the caller.

`tcp::connect_options::socket` and `tcp::listen_options::socket` share `pace::socket_options`. Fields select the interface index, mark, FreeBSD routing table, keepalive and its idle time, interval and probe count, TCP user timeout, DF, Fast Open and Linux transparent bind. Unset fields keep the OS defaults. Requesting an unavailable feature reports an error. With Fast Open enabled, TCP exchange can send the first request bytes during the handshake. Use it for replayable requests. Linux can defer the SYN until the first write. Darwin uses `connectx`, FreeBSD uses an addressed first send, and Windows uses `ConnectEx` for the exchange. A cached cookie, kernel policy and peer support determine whether data goes in the SYN.

The synchronous `pre_connect(fd)` hook runs after family selection and socket configuration, immediately before the system connect call. Throwing aborts that attempt. `original_destination()` looks up the netfilter or PF NAT mapping. PF requires access to `/dev/pf`. Linux TPROXY preserves the destination in `local_endpoint()`.

## Synopsis

```cpp
#include <pace/net.hpp>
#include <pace/runtime.hpp>

#include <array>
#include <cstddef>
#include <iostream>
#include <span>

pace::task<void> echo(pace::tcp::stream peer)
{
    std::array<std::byte, 4096> buffer{};
    while (const auto count = co_await peer.read_some(buffer))
    {
        co_await peer.write_all(std::span(buffer).first(count));
    }
}

pace::task<void> demo(pace::io_context& io)
{
    auto listener = pace::tcp::listen(io, {"127.0.0.1", 0});
    auto client = co_await pace::tcp::connect(listener.local_endpoint(), {.context = &io});
    client.set_no_delay();
    io.spawn(echo(co_await listener.accept()));

    co_await pace::write(client, "Hello, TCP!");
    std::cout << co_await pace::read_exact(client, 11) << '\n';
    co_await client.close_gracefully();
}

int main()
{
    pace::run(demo);
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

| Member | Meaning |
|:--|:--|
| `address` | Numeric, unbracketed IPv4 or IPv6 address, such as `127.0.0.1`, `::1` or `::ffff:192.0.2.1` |
| `port` | TCP port. `0` on a listener asks the OS for a free port |
| `family` | `unspecified` infers the family from `address`; `ipv4` or `ipv6` must match it |
| `scope_id` | IPv6 interface index for scoped (link-local) addresses; must be `0` for IPv4 |

`{address, port}` initializers are enough for most uses. Host names, brackets, embedded NUL bytes, malformed
addresses, a family that does not match the address and a nonzero scope on IPv4 throw `std::invalid_argument`
when the endpoint is used. Textual zone suffixes such as `fe80::1%eth0` or `fe80::1%3` are rejected: write
`{"fe80::1", port, pace::address_family::ipv6, 3}` instead. Choosing the interface index and the reachability of
link-local addresses are up to the application.

Endpoints returned by `local_endpoint()` and `remote_endpoint()` carry an owned numeric address, the actual port,
the concrete family (never `unspecified`) and the scope.

### `listen_options`

```cpp
struct listen_options
{
    bool reuse_port = false;
    int backlog = 1024;
    bool ipv6_only = true;
    pace::socket_options socket{};
};
```

| Member | Meaning |
|:--|:--|
| `reuse_port` | Request native same-port listener sharing: `SO_REUSEPORT_LB` where available, otherwise `SO_REUSEPORT`. Throws `std::system_error` with `std::errc::operation_not_supported` where neither exists (Windows) |
| `backlog` | Length of the pending-connection queue passed to `listen()`. Must be positive; the OS may cap it |
| `ipv6_only` | On an IPv6 endpoint, sets `IPV6_V6ONLY`. `false` accepts IPv4 and IPv6 on one wildcard listener. Must stay `true` on an IPv4 endpoint |

`reuse_port` lets several listeners (for example one per worker context) accept on one port; the kernel spreads
new connections among them. It does not move established connections between contexts. Independently of this
option, PACE sets `SO_REUSEADDR` on POSIX listeners and `SO_EXCLUSIVEADDRUSE` on Windows.

IPv6 listeners always set `IPV6_V6ONLY` explicitly, whatever the OS default, so a separate IPv4 listener can bind
the same port. With `ipv6_only = false` (for example `listen(io, {"::", port}, {.ipv6_only = false})`) PACE sets
`IPV6_V6ONLY=0` before binding and reports a failure instead of falling back. IPv4 peers of such a listener are
reported as IPv4-mapped IPv6 addresses (`::ffff:127.0.0.1`); access control and logging must handle that form.

### `stream`

```cpp
class stream
{
public:
    explicit stream(io_context& context);
    stream(io_context& context, address_family family);
    ~stream();
    stream(stream&& other) noexcept;
    stream& operator=(stream&& other) noexcept;
    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;

    task<void> connect(endpoint endpoint, connect_options options = {});
    task<std::size_t> read_some(std::span<std::byte> buffer);
    task<void> read_exact(std::span<std::byte> buffer);
    task<std::string> read_exact(std::size_t size, std::size_t maximum = std::size_t{1024} * 1024);
    task<void> write_all(std::span<const std::byte> bytes);
    task<void> shutdown_write();

    void set_no_delay(bool enabled = true);
    task<void> close_gracefully(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(100),
        std::size_t discard_limit = std::size_t{64} * 1024
    );
    void close() noexcept;

    [[nodiscard]] native_socket_type native_handle() const noexcept;
    [[nodiscard]] io_context& context() const noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] endpoint local_endpoint() const;
    [[nodiscard]] endpoint remote_endpoint() const;
};
```

A move-only TCP connection owned by one `io_context`. Streams come from `tcp::connect`, `listener::accept`, or
construction followed by `connect`.

#### Construction

`stream(io)` creates an unconnected nonblocking IPv4 socket and picks the family on the first `connect`: an IPv6
destination replaces the handle with an IPv6 socket, keeping a `set_no_delay` setting. `stream(io, family)` with
`ipv4` or `ipv6` keeps its handle and rejects a destination of the other family; use it when you set other native
options on `native_handle()` before connecting. Outbound IPv6 sockets set `IPV6_V6ONLY=0`, so IPv4-mapped
destinations work on every platform. Construction may run before the context runs and throws `std::system_error`
if the OS cannot create the socket.

#### `connect`

```cpp
task<void> connect(endpoint endpoint, connect_options options = {});
```

Connects to a numeric endpoint. Await it before starting any other operation on the stream. Completion is checked
through `SO_ERROR`; refusal and other failures throw `std::system_error`. A stream allows one connection attempt:
once the native attempt has started, another `connect` throws `std::logic_error`, so after a failure close the
stream and use a new one. An endpoint rejected during validation (before the native attempt) leaves the stream
usable for a corrected endpoint. There is no connect timeout; use a
[deadline](../runtime/structured-concurrency.md).

#### Reading

```cpp
task<std::size_t> read_some(std::span<std::byte> buffer);
```

Waits until at least one byte is available and returns how many bytes were copied, possibly fewer than
`buffer.size()`. With a nonempty buffer, `0` means the peer closed its sending side (orderly EOF). An empty buffer
returns `0` without reading. The bytes are not NUL-terminated. Read into a character buffer through
`pace::as_writable_bytes`:

```cpp
std::array<char, 4096> buffer{};
const auto count = co_await stream.read_some(pace::as_writable_bytes(buffer));
std::string_view received(buffer.data(), count);
```

```cpp
task<void> read_exact(std::span<std::byte> buffer);
task<std::string> read_exact(std::size_t size, std::size_t maximum = std::size_t{1024} * 1024);
```

Members with the same behaviour as the free [`pace::read_exact`](README.md#read_exact): fill exactly the requested
length without consuming the following bytes, throw `std::system_error` with `std::errc::connection_reset` on EOF
before the end, and (for the `size` overload) throw `std::length_error` when `size > maximum` before allocating.
A failed span read may have overwritten a prefix of the buffer.

#### Writing

```cpp
task<void> write_all(std::span<const std::byte> bytes);
```

Sends the whole span, looping over partial sends, and completes when the OS has accepted every byte. That does not
mean the peer has received or processed them. The task borrows the bytes: keep them alive and unchanged until it
completes, or use [`pace::write`](README.md#write), which owns a string. Text is passed as
`write_all(pace::as_bytes(text))`. An empty span completes without sending. Large writes yield to the reactor after
each MiB so other connections make progress. On POSIX, sends never raise `SIGPIPE`; a closed peer is reported as
`std::system_error`.

#### `shutdown_write`

```cpp
task<void> shutdown_write();
```

Half-closes the connection: sends FIN (`shutdown` of the send side) so the peer reads EOF, while this side keeps
reading until the peer's own EOF. It completes at once, without waiting for the peer, and throws
`std::system_error` on an OS failure or a closed stream. Do not call it while a write is outstanding; a later
`write_all` fails with the OS error. Unlike `close_gracefully`, it neither drains nor closes the socket: read the
reply, then `close()` or destroy the stream.

```cpp
co_await stream.write_all(pace::as_bytes(request));
co_await stream.shutdown_write(); // the peer reads EOF after the request
std::string reply;
std::array<char, 4096> buffer{};
while (const auto count = co_await stream.read_some(pace::as_writable_bytes(buffer)))
{
    reply.append(buffer.data(), count);
}
```

#### `set_no_delay`

```cpp
void set_no_delay(bool enabled = true);
```

Sets (`true`) or clears (`false`) `TCP_NODELAY`. Call it before the context runs or on the owning context. Throws
`std::system_error` on a closed stream, a stopped context or an OS failure.

#### Bounded graceful close

```cpp
task<void> close_gracefully(
    std::chrono::milliseconds timeout = std::chrono::milliseconds(100),
    std::size_t discard_limit = std::size_t{64} * 1024
);
```

Half-closes the sending side, then reads and discards incoming bytes until the peer's EOF, `timeout` or
`discard_limit` bytes, whichever comes first, and closes the socket. This lets a final response reach the peer even
when the peer still has unread request bytes in flight, which would otherwise make the close send a reset.

- Await it only when no other operation is outstanding on the stream.
- Reaching `timeout` or `discard_limit` is normal completion, not an error.
- A zero or negative `timeout`, or a zero `discard_limit`, closes right after the half-close.
- The socket is closed in every case: success, a bound reached, cancellation and errors. Errors and cancellation
  still propagate after the close.
- The bound limits the cooperative drain only. It cannot guarantee that the peer received the data; a peer that
  keeps sending may still observe an abrupt close.

#### `close`

```cpp
void close() noexcept;
```

Cancels the stream's pending waits and closes the native handle; `is_open()` becomes `false`. Idempotent. Suspended
operations are resumed with `std::errc::operation_canceled` (or `bad_file_descriptor`) afterwards, so the `stream`
object itself must stay alive until they have finished. Destruction calls `close()`.

#### Accessors

| Member | Result |
|:--|:--|
| `native_handle()` | The borrowed native socket. Do not close it or change its blocking mode |
| `context()` | The owning `io_context` |
| `is_open()` | Whether the stream owns a handle; says nothing about the connection's health |
| `local_endpoint()` | The bound local address and port, from the native socket |
| `remote_endpoint()` | The connected peer's address and port; requires a connected stream |

`local_endpoint()` and `remote_endpoint()` throw `std::system_error`: `bad_file_descriptor` on a closed stream,
`operation_canceled` on a stopped context, or the native error (for example when the peer address is not
available).

### `listener`

```cpp
class listener
{
public:
    listener(io_context& context, endpoint endpoint, listen_options options = {});
    ~listener();
    listener(listener&& other) noexcept;
    listener& operator=(listener&& other) noexcept;
    listener(const listener&) = delete;
    listener& operator=(const listener&) = delete;

    task<stream> accept();
    [[nodiscard]] endpoint local_endpoint() const;
    [[nodiscard]] io_context& context() const noexcept;
    [[nodiscard]] native_socket_type native_handle() const noexcept;
    void close() noexcept;
};
```

A move-only listening socket. The constructor creates, binds and listens synchronously, and may run before the
context runs. Validation errors throw `std::invalid_argument`, OS errors `std::system_error`; a failed constructor
closes any handle it created. Port `0` binds a free port; read it back with `local_endpoint()`. `0.0.0.0` binds
every IPv4 interface and `::` every IPv6 interface.

`accept()` waits for one connection and returns a new connected `stream` owned by the listener's context. Accepted
sockets are nonblocking. Connections aborted or reset before they were accepted are skipped. When the process runs
out of descriptors or socket buffers, `accept()` backs off for 50 ms and retries instead of failing. After 64
accepts in a row it yields to the reactor so other work runs. Other errors throw `std::system_error`.

`close()` cancels a pending `accept()`, which then throws `std::system_error` with `operation_canceled` or
`bad_file_descriptor`. Streams already accepted are not affected. A listener has no `is_open()`; compare
`native_handle()` with `pace::invalid_socket` if needed. `local_endpoint()`, `context()`, `native_handle()`, moves and
destruction follow the `stream` rules.

## Functions

### `connect`

```cpp
task<stream> connect(endpoint destination, client_options options = {});
task<result<stream>> connect(as_result_t, endpoint destination, client_options options = {});
```

Connects to the numeric endpoint and returns a stream owned by the running context. An explicit `options.context` must match the context executing the task. Keep the context alive while using the stream.

### `listen`

```cpp
listener listen(io_context& io, endpoint local, listen_options options = {});
listener listen(endpoint local, listen_options options = {});
```

Constructs a `listener`: binds and listens immediately. The overload without a context calls
`pace::this_context()` at the call, so it must be called from a task or callback on a running context; use the
explicit-context overload to create a listener before `io.run()`.

### `exchange` and `async_exchange`

```cpp
task<std::string> async_exchange(endpoint peer, std::string request, std::size_t reply_size, client_options options = {});
std::string exchange(endpoint peer, std::string request, std::size_t reply_size, client_options options = {});
```

One connection, one request, one reply of exactly `reply_size` bytes: connect to `peer`, write `request`, read
exactly `reply_size` bytes, close gracefully (with the default 100 ms / 64 KiB bounds) and return the reply. There is
no pooling or reuse.

- `reply_size` is an exact framing contract, not a maximum: fewer bytes followed by EOF is an error. `0` is allowed;
  the request is still sent.
- `reply_size > options.max_reply_size.value_or(1024 * 1024)` throws `std::length_error` before connecting.
- Request and reply strings may contain zero bytes.
- `async_exchange` inherits cancellation and deadlines. Set `options.timeout` or `options.deadline` to bound the entire exchange, including graceful close.
- `exchange` runs on a fresh context with a 30 s default total timeout. Expiry reports `timed_out`. Calling it on a reactor thread throws `std::logic_error`.

For persistent connections, other framing (delimiters, length prefixes) or reusable buffers, use `connect`,
`pace::write` and `pace::read_exact` on an owned stream.

## Errors

| Condition | Exception |
|:--|:--|
| Malformed endpoint, host name, `%zone`, family or scope mismatch | `std::invalid_argument` |
| `listen_options::backlog <= 0`, or `ipv6_only = false` on an IPv4 endpoint | `std::invalid_argument` |
| `connect` to the other family on a stream created with an explicit family | `std::system_error` (`address_family_not_supported`) |
| Second `connect` on one stream | `std::logic_error` |
| OS socket error (refused, reset, unreachable, …) | `std::system_error` with the native code |
| EOF inside `read_exact` / `exchange` | `std::system_error` (`connection_reset`) |
| `read_exact` / `exchange` size above `maximum` | `std::length_error` |
| Operation on a closed stream or listener | `std::system_error` (`bad_file_descriptor`) |
| Context stopped, stream closed during the wait, or task canceled | `std::system_error` (`operation_canceled`) |
| Deadline of the awaiting task, or `exchange` budget, expired | `std::system_error` (`timed_out`) |
| Operation run on a different running `io_context` | `std::logic_error` |
| Second outstanding operation in one direction | `std::logic_error` |
| `reuse_port` where unsupported | `std::system_error` (`operation_not_supported`) |

Native errors use `std::generic_category()` on POSIX and `std::system_category()` on Windows; PACE's own
conditions use portable `std::errc` codes. Compare with conditions, for example
`error.code() == std::errc::operation_canceled`, not with numeric `errno` values. Catch per-connection errors in
the connection's task so one failing peer does not end the program.

## Threads and concurrency

- Run every operation, `set_no_delay`, `close` and the accessors on the owning context's thread, or before the
  context runs or after it has stopped. To close a stream from another thread, `post` the call to its context.
  PACE checks some misuse (an operation run on a different running context throws `std::logic_error`) but not all.
- A stream allows one outstanding read (`read_some`, `read_exact`) and one outstanding write (`write_all`,
  `connect`) at the same time, so a reader task and a writer task may share a stream. A second read or write
  started while one in the same direction is suspended throws `std::logic_error` at once, before it touches the
  socket, so it can neither take the first read's data nor interleave its bytes with the first write.
- A listener allows one outstanding `accept()`; a second one throws `std::logic_error` at once.
- `close_gracefully()` requires that nothing else is outstanding. `shutdown_write()` belongs to the write
  direction: call it when no write is outstanding (this is not checked); a read may still be outstanding.

## Lifetime and ownership

- The `io_context` must outlive every stream and listener created on it.
- Keep a stream or listener alive, and at the same address, while any of its operations is suspended. Closing
  resumes them, but destroying the object before they have finished is undefined behaviour.
- Buffers passed to `read_some`, `read_exact` (span overload) and `write_all` are borrowed until the operation
  completes, and so is the storage behind a `pace::as_bytes` or `pace::as_writable_bytes` view. `connect` and the helpers taking `std::string` own their arguments.
- Move a stream or listener only when no operation is outstanding. A move leaves the source without a handle (but
  still bound to its context); move assignment closes the destination's previous socket first.
- An accepted stream belongs to the listener's context; there is no way to rebind it to another context.

## Limits and defaults

| Item | Default or bound |
|:--|:--|
| `endpoint::address` / `port` / `family` / `scope_id` | `"127.0.0.1"` / `0` / `unspecified` / `0` |
| `listen_options::backlog` | 1024 (must be > 0; the OS may cap it) |
| `listen_options::reuse_port` / `ipv6_only` | `false` / `true` |
| `close_gracefully` timeout / discard limit | 100 ms / 64 KiB |
| `read_exact` maximum / `exchange` max_reply_size | 1 MiB |
| `exchange` timeout (`client_options::timeout`) | 30 s |
| `write_all` bytes sent before yielding to the reactor | 1 MiB |
| `accept()` consecutive accepts before yielding | 64 |
| `accept()` back-off on descriptor or buffer exhaustion | 50 ms |
| Connect timeout, read or write timeout | none; use a deadline |

## See also

- [Networking overview](README.md), [UDP](udp.md)
- [TLS](../tls/README.md) over TCP, [DNS](../dns/README.md) for host names
- [I/O context](../runtime/io-context.md), [running a program](../runtime/run.md),
  [structured concurrency](../runtime/structured-concurrency.md)
- Tutorial: [TCP](../../tutorial/05-tcp.md), [graceful shutdown](../../tutorial/09-graceful-shutdown.md)
- Guide: [protocol tuning](../../guides/protocol-tuning.md)
