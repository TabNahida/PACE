# Component conveniences (0.3.1)

[中文](component-conveniences.zh-CN.md) · [Tutorials](../tutorial/README.md) · [Tuning](protocol-tuning.md)

These additions require **PACE v0.3.1 or newer**. Use the
[package quick start](../tutorial/00-quick-start.md) or build from source using
[chapter 1](../tutorial/01-build.md).
Existing low-level methods remain available. Optional modules keep their existing
build flags and dependency boundaries.

## Context and lifetime

No helper starts an event loop, thread or worker pool. Overloads without an
`io_context&` use `pace::this_context()`. Async helpers do this when their lazy
task executes; `tcp::listen`, `quic::listen`, `udp::open` and `udp::bind` are synchronous and need
a running context **when called**. Use explicit-context overloads to construct
listeners/sockets before `io.run()`. Outside a loop, pass a lazy task to
`pace::run`, for example `pace::run(dns::resolve("127.0.0.1"))`.

Values passed to tasks are owned. Streams passed by reference, spans and custom
TLS configurations are borrowed. Keep borrowed objects alive and unmoved until
the await finishes. A context must outlive every object bound to it; do not return
sockets from `pace::run`, whose internal context ends on return. Helpers preserve
the underlying operation/concurrency/cancellation contracts, not an extra lock or
queue. They add no general timeout, retry, pooling or DNS policy.

## Byte streams and TCP

Include `<pace/stream_io.hpp>`; TCP, TLS and QUIC headers include it too.
Supported streams are `tcp::stream`, `tls::stream` and `quic::stream`:

```cpp
template<class Stream> task<void> write(Stream&, std::string bytes);
template<class Stream> task<void> read_exact(Stream&, std::span<char>);
template<class Stream> task<void> read_exact(Stream&, std::span<std::byte>);
template<class Stream> task<std::string> read_exact(
    Stream&, std::size_t size, std::size_t maximum = 1024 * 1024);
```

`write` holds the string through `write_all`; an lvalue string is copied and a
moved string is transferred. Binary data is supported (give the string its exact
length). `read_exact` fills only the specified length, preserving subsequent bytes.
The owning overload rejects `size > maximum` with `length_error` before allocating
or reading; the span overload has no extra allocation bound. Empty exact reads
complete without reading. Early EOF throws `system_error(connection_reset)`.
Failed span reads may have modified a prefix. Cancellation and I/O errors propagate.
These operations do not invent message framing or guarantee peer processing.

In `<pace/tcp.hpp>`, linking `pace_net`:

```cpp
tcp::listener tcp::listen(io_context&, tcp::endpoint, tcp::listen_options = {});
tcp::listener tcp::listen(tcp::endpoint, tcp::listen_options = {});
```

Listeners bind immediately and retain existing backlog/family behavior. Use the
v0.3.0 `tcp::connect([io,] endpoint)` for a connected stream. Numeric endpoints
support IPv4/IPv6; there is no implicit name lookup. Existing member
`tcp::stream::read_exact` remains supported. [TCP contracts](tcp.md).

## UDP

Include `<pace/udp.hpp>` and link `pace_net`:

```cpp
udp::socket udp::open(io_context&);
udp::socket udp::open();
udp::socket udp::bind(io_context&, udp::endpoint, udp::bind_options = {});
udp::socket udp::bind(udp::endpoint, udp::bind_options = {});
struct udp::packet { std::string data; udp::endpoint sender; bool truncated = false; };
// Members of udp::socket:
task<std::size_t> send(std::string bytes, udp::endpoint destination);
task<udp::packet> receive(std::size_t capacity = 65536);
```

`open` is initially unbound and selects the family on first send. `bind` binds
immediately; port zero chooses an OS-assigned port. `send` owns its string and
returns the sent byte count. `receive` allocates storage for `capacity`, returns
only the received bytes, and owns its data independently of the socket. A zero-byte
message is valid. `capacity == 0` throws `invalid_argument`; capacity above 65536
throws `length_error`, before I/O. A smaller capacity may truncate a datagram;
check `truncated`, since its tail has already been discarded. The default capacity
is an allocation bound, not a promise that the network accepts 64 KiB payloads.

One send and one receive may overlap; two sends or two receives may not. Closing
cancels pending operations, but the object must stay alive until they unwind.
`send_to`/`receive_from` keep their borrowed-buffer behavior. [UDP contracts](udp.md).

## DNS

Include `<pace/dns.hpp>`, enable `--dns=y`, link `pace_dns`:

```cpp
task<dns::result> dns::resolve(io_context&, std::string hostname,
    dns::family = dns::family::any, dns::options = {});
task<dns::result> dns::resolve(std::string hostname,
    dns::family = dns::family::any, dns::options = {});
```

Each task owns a resolver, hostname and options through completion. Defaults are
system nameservers, a 5-second whole-lookup timeout and two tries. Results and
c-ares error codes match `resolver::resolve`; numeric lookups need no nameserver.
Context stop cancels waits. Use an explicit `dns::resolver` when you need its
`cancel()` handle. This does not choose a service port, connect or try multiple
addresses. [DNS options and errors](dns.md).

## TLS

Include `<pace/tls.hpp>`, enable `--tls=y`, link `pace_tls`:

```cpp
task<tls::stream> tls::connect(io_context&, const tls::context&,
    tcp::endpoint, std::string expected_name);
task<tls::stream> tls::connect(const tls::context&, tcp::endpoint, std::string expected_name);
task<tls::stream> tls::connect(io_context&, tcp::endpoint, std::string expected_name);
task<tls::stream> tls::connect(tcp::endpoint, std::string expected_name);
task<tls::stream> tls::accept(tcp::stream, const tls::context&);
```

All return a stream only after the handshake. `accept` owns its TCP transport.
Custom configs are **borrowed through task completion**: do not store a lazy task
that refers to a destroyed temporary config. Returned streams retain shared config
state. Config-free clients own a default system-trust config; they do not select
ALPN, disable verification or download roots. Configure custom trust/ALPN before
first use. The expected certificate name is required separately from the numeric
endpoint; DNS names also set SNI. Only one asynchronous TLS stream operation may
be active, including reads versus writes. [TLS contracts](tls.md).

## WebSocket

Include `<pace/websocket.hpp>`, enable `--websocket=y`, link `pace_websocket`:

```cpp
template<class Transport> task<websocket::basic_stream<Transport>> websocket::upgrade(
    Transport, std::string authority, std::string target = "/",
    std::vector<std::string> protocols = {}, websocket::limits = {});
template<class Transport> task<websocket::basic_stream<Transport>> websocket::accept(
    Transport, std::vector<std::string> protocols = {},
    std::function<bool(const websocket::upgrade_request&)> authorize = {},
    websocket::limits = {});
task<websocket::stream> websocket::connect(io_context&, tcp::endpoint,
    std::string authority, std::string target = "/",
    std::vector<std::string> protocols = {}, websocket::limits = {});
// Also connect(endpoint, authority, target, protocols, limits) on the current context.
```

The transport is owned TCP or already handshaken TLS (`--tls=y`). Handshake inputs
and the authorization callback are owned too; references captured by that callback
remain borrowed. The helper selects the client/server role and completes its
handshake. `connect` includes TCP connection; `upgrade` composes with TLS.
Authority is the Host value, including a port when required, not a URL. These
helpers do not parse `ws://` URLs or resolve names. Missing authorization callback
means no application authorization check. Use member `accept` if you also need
its returned upgrade metadata; the helper returns just the socket.

Only one async operation per WebSocket stream is permitted, including receive,
send and close. [WebSocket contracts](websocket.md).

## QUIC

Include `<pace/quic.hpp>`, enable `--quic=y`, link `pace_quic` (POSIX):

```cpp
task<quic::connection> quic::connect(io_context&, quic::context,
    udp::endpoint, std::string expected_name,
    std::chrono::milliseconds timeout = std::chrono::seconds(10));
// Also connect(config, endpoint, expected_name, timeout) on the current context.
```

`quic::listen([io,] endpoint, server_config)` binds synchronously and returns a
listener retaining shared configuration, with explicit/current-context overloads.

The task owns a copy of the lightweight shared config and returns after the
verified handshake. Configure ALPN and trust explicitly before use. The timeout
bounds connection establishment; it is not a stream/request deadline. A nonpositive
bound permits one immediate handshake attempt, matching `connection::connect`.
The connection must remain open while streams operate; retain the listener while
its accepted connections operate. [QUIC contracts](quic.md).

## HTTP/2 and HTTP/3

Enable the matching module and link `pace_http2` or `pace_http3`:

```cpp
template<class Transport> task<http::response> http2::get(
    Transport, std::string authority, std::string target = "/", http2::limits = {});
task<http::response> http3::get(
    quic::connection, std::string authority, std::string target = "/", http3::limits = {});
```

Each helper owns the connected transport for a single GET batch, delegates to
`exchange`, and returns the first response. HTTP/2 takes prior-knowledge TCP or
verified TLS with ALPN `h2`. HTTP/3 takes verified QUIC with ALPN `h3`. These are
path/authority APIs, not URL clients; there is no fallback or implicit DNS. Bodies
are buffered and existing limits apply. For custom headers, other methods or
multiple multiplexed requests use `exchange`. The transferred connection is not
available for reuse by the caller after GET. [HTTP/2](http2.md) · [HTTP/3](http3.md).
