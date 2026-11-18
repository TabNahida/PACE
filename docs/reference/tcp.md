# `pace::tcp`

For one-call `exchange` / `async_exchange` clients, see [direct clients](direct-clients.md).

Short forms and owning helpers: [component conveniences](component-conveniences.md).

[0.3.0 convenience additions](convenience.md)

Include `<pace/tcp.hpp>`. TCP sockets are nonblocking, move-only and tied to an
`io_context`. Operations return lazy [tasks](task.md). Keep the context alive
longer than its sockets and listeners, and keep each socket object and operation
buffer alive until the operation completes.

## Addresses and listener options

```cpp
namespace pace::tcp {
struct endpoint {
    std::string address = "127.0.0.1";
    std::uint16_t port = 0;
    pace::address_family family = pace::address_family::unspecified;
    std::uint32_t scope_id = 0;
};
struct listen_options {
    bool reuse_port = false;
    int backlog = 1024;
    bool ipv6_only = true;
};
}
```

An endpoint owns its numeric IPv4 or IPv6 address, such as `127.0.0.1` or `::1`.
Existing `{address, port}` initializers remain valid. `pace::address_family`,
declared in `<pace/ip.hpp>`, has `unspecified`, `ipv4` and `ipv6` values. Input
defaults to automatic inference; an explicit family must match the address.
Endpoint inspection always returns the concrete family. Hostnames, brackets,
embedded NUL bytes and malformed addresses throw `std::invalid_argument`.
Resolve hostnames explicitly with the [DNS extension](dns.md).

For scoped IPv6, set `scope_id` to the numeric interface index; native endpoint
inspection preserves that index separately from the address string. A nonzero
scope on IPv4 is invalid. Textual zone suffixes such as `fe80::1%eth0` or
`fe80::1%3` are explicitly rejected: use `{"fe80::1", port,
pace::address_family::ipv6, 3}`. Interface-index selection and link-local
reachability are the application's responsibility.

A listener's port `0` requests an OS-assigned port, retrievable through
`local_endpoint()`. `0.0.0.0` binds all local IPv4 interfaces and `::` binds all
local IPv6 interfaces. IPv6 listeners explicitly set `IPV6_V6ONLY=1` by default,
so a separate IPv4 listener can bind the same port. To accept both families on
one wildcard listener, use `listener(io, {"::", port}, {.ipv6_only = false})`.
PACE sets `IPV6_V6ONLY=0` before bind and propagates native failures; it does not
silently fall back to another policy. This option is invalid on an IPv4 endpoint.
An accepted IPv4 peer is reported as IPv6 with an IPv4-mapped address, for example
`::ffff:127.0.0.1`. Access-control and logging code must handle mapped addresses.
Use `::1` or `127.0.0.1` for examples intended to stay local.

`backlog` must be positive or construction throws `invalid_argument`. The OS can
cap the requested queue length. `reuse_port` requests native same-port listener
sharing (`SO_REUSEPORT_LB` where available, otherwise `SO_REUSEPORT`). It is not
portable on Windows and throws `system_error(operation_not_supported)` where
unsupported. Even where available, native errors remain possible. It distributes
new connections among separate listeners; it does not migrate established sockets
between contexts. PACE sets `SO_REUSEADDR` on POSIX listeners and exclusive address
use on Windows independently of this option.

## `stream`: ownership and accessors

```cpp
explicit stream(io_context& context);
stream(io_context& context, pace::address_family family);
~stream();
stream(stream&& other) noexcept;
stream& operator=(stream&& other) noexcept;
stream(const stream&) = delete;
stream& operator=(const stream&) = delete;

native_socket_type native_handle() const noexcept;
io_context& context() const noexcept;
bool is_open() const noexcept;
endpoint local_endpoint() const;
endpoint remote_endpoint() const;
void close() noexcept;
```

Construction creates an unconnected nonblocking socket and may throw a native
`system_error`. It may happen before the context runs. The default constructor
starts with IPv4 and chooses the destination family on the first `connect`.
For IPv6 it replaces the initial handle while preserving `set_no_delay()`.
Use `stream(io, pace::address_family::ipv6)` or `ipv4` when configuring other
native socket options before connect: an explicit family keeps the handle and
rejects a destination from the other family. Outbound IPv6 sockets support
IPv4-mapped numeric destinations consistently through `IPV6_V6ONLY=0`.
`context()` returns the
same owning context; it does not transfer ownership. `native_handle()` returns a
borrowed handle; do not independently close it or change nonblocking state.
`is_open()` reports ownership of a handle, not whether it is connected or healthy.

`local_endpoint()` and `remote_endpoint()` query the native socket and return an
owned numeric address, actual port, concrete family and scope. The remote query
requires a connected stream. Query before run or on the owning loop, without
concurrent close/move; native unavailable-endpoint errors propagate. Closed
sockets throw `bad_file_descriptor` (a stopped context takes precedence).

Move only when no operation references the source or destination object. A move
empties the source handle while retaining its context association. Move assignment
closes the destination's previous socket. Destruction calls `close()`.

`close()` is idempotent. On the owning loop it cancels registered waits before
closing the native handle, leaving `is_open()` false. Construction/setup before
run and cleanup after run joins are supported. Do not close or destroy a live
socket concurrently from another thread. Closing schedules suspended operations
to resume; **destroying the C++ object must wait for those member operations to
finish**. Use `context.post(...)` with a lifetime-safe reference to request close
from another thread.

## Connecting, reading and writing

```cpp
task<void> connect(endpoint endpoint);
task<std::size_t> read_some(std::span<char> buffer);
task<std::size_t> read_some(std::span<std::byte> buffer);
task<void> write_all(std::string_view bytes);
task<void> write_all(std::span<const std::byte> bytes);
void set_no_delay(bool enabled = true);
```

`connect` owns its endpoint value. Await it on the stream's context, before
starting other operations on that stream. Native pending connection completion is
checked through `SO_ERROR`; refusal and other failures throw `system_error`.
There is no built-in connect timeout or hostname resolution. Once a native
connection attempt has started, another `connect` on that stream throws
`logic_error`. After connection failure, close the socket and create a fresh
stream for a retry. Endpoint/family validation failures before a native attempt
leave the stream available for a corrected endpoint.

`read_some` waits until at least some bytes are available, an error occurs, or EOF
is observed. It can return fewer bytes than the buffer size. With a **nonempty**
buffer, zero means orderly EOF; an empty buffer also returns zero without a read.
The supplied memory must remain valid and writable for the entire operation.
Returned bytes are not NUL-terminated. There is no built-in `read_exactly` or
delimiter parser; loop and track the remaining span when framing a protocol.

`write_all` loops over partial native sends until it accepts the complete view or
fails. It does not promise that the peer has received or processed those bytes.
Keep the backing string/array alive and unchanged until completion. The task owns
the view, not its bytes. Empty views complete without a send after checking socket
state. The byte-span overloads adapt to the same implementation without copying
the payload. Individual native read/send sizes are capped at `INT_MAX`, and
`write_all` advances until all requested data has been accepted.

`set_no_delay(true)` enables `TCP_NODELAY`; `false` disables it. Call before run or
on the owning loop. It throws on invalid/stopped state or native option failure.
It does not imply an application batching policy or a performance guarantee.

At most one read operation and one write operation may be outstanding at once.
A connect uses the write side; an accept uses the listener's read side. Two
same-direction operations are not supported even if one happens to complete
synchronously. Await one before starting another. Read and write buffers are
independently owned by the application.

## Bounded graceful close

```cpp
task<void> close_gracefully(
    std::chrono::milliseconds timeout = std::chrono::milliseconds(100),
    std::size_t discard_limit = 64 * 1024);
```

Await this only when **no other socket operations are outstanding**. It half-closes
the send direction, then reads and discards incoming bytes until peer EOF, the
time allowance or the discard allowance is reached. It waits for socket readiness
with one cancellable deadline; successful completion removes that timer immediately.
The socket is closed on success, timeout/limit
completion, cancellation and errors. Native errors and cancellation still
propagate after close. This supports delivering a final response even when unread
request data would otherwise cause an immediate reset.

Zero/negative timeout or a zero discard allowance skips draining after the send
shutdown. Reaching a bound is normal completion, not a timeout exception. The
deadline bounds cooperative draining, not OS scheduling or a peer's delivery
acknowledgment; a peer that does not cooperate may still see an abrupt final close.
It is not a general I/O operation deadline.

## `listener`

```cpp
listener(io_context& context, endpoint endpoint, listen_options options = {});
~listener();
listener(listener&& other) noexcept;
listener& operator=(listener&& other) noexcept;
listener(const listener&) = delete;
listener& operator=(const listener&) = delete;

task<stream> accept();
endpoint local_endpoint() const;
io_context& context() const noexcept;
native_socket_type native_handle() const noexcept;
void close() noexcept;
```

Construction synchronously creates, binds and listens on a nonblocking socket.
Address/option validation and native socket errors can throw; failed construction
closes any created handle. `local_endpoint()` returns an owned numeric address
and actual bound port and throws on a closed/invalid native socket. Access it
before run or on the owner without concurrent close/move.

`accept()` waits for one connection and returns a new owning `stream` bound to the
**listener's context**. Accepted sockets are configured nonblocking. Certain
transient aborted/reset accepts are retried; other failures propagate. There is
one outstanding accept per listener. The listener must stay alive and unmoved
while accept is suspended. There is no accepted-socket context-rebinding API.

Listener move, destruction, `context`, `native_handle` and `close` follow the same
ownership/affinity rules as the stream equivalents. Closing a listener cancels
its accept wait; it does not close already accepted streams. A listener has no
`is_open()` member; compare its handle with `pace::invalid_socket` if needed.

## Error and cancellation behavior

TCP operation bodies check stopped context first (`operation_canceled`), then
closed handle (`bad_file_descriptor`), and reject execution under a different
running PACE context with `logic_error`. Readiness registration also requires the
owning context. This is not a guarantee that every misuse from an unrelated
thread is detected; obey the affinity contract regardless of checks.

Socket failures use `std::system_error`: native errors use `generic_category` on
POSIX and `system_category` on Windows. Library cancellation and invalid-state
errors use portable `std::errc` codes. Compare conditions such as
`error.code() == std::errc::operation_canceled`; do not assume numeric errno values
on Windows. POSIX sends suppress `SIGPIPE` through supported per-send/per-socket
mechanisms. The caller must catch expected per-peer errors so one disconnected
client does not become an unhandled root failure.

See the [TCP tutorial](../tutorial/03-tcp.md) for complete client and server code.
