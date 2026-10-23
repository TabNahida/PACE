# QUIC transport

Enable `--quic=y` and include `<pace/quic.hpp>`. This optional Layer 2 extension
uses OpenSSL 3.5 or newer for QUIC v1 and its TLS 1.3 handshake, loss recovery,
congestion control, flow control and packet encryption. The build exports
`PACE_HAS_QUIC`; HTTP/3 is a separate application module in `<pace/http3.hpp>`.

```sh
xmake f -m release --quic=y --tests=y --examples=y -y
xmake -y
xmake run pace_quic_tests
xmake run tutorial_quic
```

The [complete example](../../examples/tutorial_quic.cpp) exchanges data over a
verified local connection. The checked-in CA and private key are public test
fixtures and must not be deployed.

## Configuration and establishment

```cpp
pace::quic::context server_config(pace::quic::role::server), client_config;
server_config.use_certificate_chain("server-chain.pem", "server-key.pem");
client_config.load_verify_file("private-ca.pem");
server_config.set_alpn_protocols({"my-protocol"});
client_config.set_alpn_protocols({"my-protocol"});

pace::quic::listener listener(io, {"0.0.0.0", 4433}, server_config);
auto accepted = co_await listener.accept();

pace::quic::connection client(io, client_config);
co_await client.connect({"127.0.0.1", 4433}, "localhost");
```

Clients always verify certificate trust and the supplied DNS name or IP subject
alternative name. The network address is independent from the expected certificate
identity. An empty identity or embedded NUL is rejected. DNS identities send SNI;
IPv4 and IPv6 identities check IP SANs. System trust paths are loaded by default, and
`load_verify_file()` adds a PEM trust file. There is no insecure client mode.
Server keys must be unencrypted PEM files; key loading never prompts on stdin.
Mutual TLS is not exposed in this API.

ALPN defaults to `pace-quic`. Lists must be nonempty, contain identifiers of
1–255 bytes, and occupy at most 65535 encoded bytes. Server preference determines
the selected shared protocol; no overlap fails the handshake. Read
`selected_protocol()` after establishment. Configure contexts before constructing
connections/listeners; their shared configuration must not be mutated concurrently
or while it is in use.

`connect(peer, identity, timeout)` and `accept(handshake_timeout)` default to a
10-second handshake deadline. The accept timeout starts when a connection is
accepted from OpenSSL; it does not bound the idle wait for a new peer. A nonpositive
timeout allows one immediate handshake attempt. Certificate/ALPN failures and
individual handshake deadlines propagate to the caller. A server can continue
accepting after such a peer failure while `listener.is_open()` is true. Reactor
cancellation and engine failures are propagated and close the engine.

Endpoints accept numeric IPv4 and IPv6 addresses. IPv6 listeners are IPv6-only;
scoped QUIC destinations are explicitly rejected because OpenSSL's public
`BIO_ADDR_rawmake` API cannot encode an interface scope. Resolve hostnames with
the separate DNS extension before connecting.

## Independent streams

```cpp
auto request = co_await client.open_stream(); // Bidirectional.
co_await request.write_all("request bytes");
request.finish_write(); // Queue FIN on this stream's sending direction.
std::array<char, 4096> buffer;
while (auto n = co_await request.read_some(buffer)) {
    consume(std::string_view(buffer.data(), n));
}

auto incoming = co_await accepted.accept_stream();
auto one_way = co_await accepted.open_stream(pace::quic::stream_type::unidirectional);
```

`id()`, `is_local()` and `is_unidirectional()` expose stream metadata. Incoming
streams become visible when the peer sends stream data or FIN; merely creating
an outgoing stream does not force immediate peer acceptance. Arrival order across
streams is not an application ordering guarantee. One reader and one writer may
be outstanding on the same stream; distinct streams may progress concurrently.
Overlapping reads, writes, opens, or accepts of the same kind throw `logic_error`.

`write_all()` accepts the complete borrowed byte sequence into OpenSSL's transport
buffers, applying backpressure when necessary. It does not imply peer receipt.
Keep input/output memory alive until the operation completes. `read_some()` on a
nonempty buffer returns zero only for stream FIN; a connection failure/reset is
an error. Empty buffers return zero without reading. `std::byte` span overloads
are also provided. Writes on the receive-only side of a unidirectional stream
and writes after FIN fail.

Nonblocking application protocol drivers can use:

- `try_open_stream(type)` and `try_accept_stream()`: an empty optional means a
  stream cannot currently be opened/accepted.
- `try_read(buffer)`: an empty optional means would-block; a zero result means FIN.
- `try_write(bytes)`: returns the number accepted, with zero meaning would-block
  for nonempty input. Retry the remaining bytes after progress.
- `wait_events()` on a connection or stream: suspends until shared transport
  progress or cancellation. Wakeups may be spurious. Recheck all relevant stream
  state after a wakeup, since one UDP socket serves many streams/connections.
- `pending_write_bytes()`: bytes still retained by OpenSSL's send buffer pending
  acknowledgement. For the supported OpenSSL implementation, subtracting this
  from cumulative accepted bytes yields the acknowledged contiguous prefix;
  this excludes FIN acknowledgement and does not prove application consumption.

Linux uses native OpenSSL datagram BIOs, UDP readiness, writable readiness when
needed, and a rearmable `timerfd` driven by OpenSSL's next deadline. There is no
fixed millisecond polling on Linux. Successful writes are coalesced into engine
ticks. Established connections use explicit event processing, so an operation
on one stream cannot silently consume packets without waking other streams.
Other POSIX builds retain a bounded 1–10 ms timer fallback. The
[platform matrix](platform.md) links to current functional validation results. Windows QUIC/HTTP3 is not implemented and enabling it
produces a build diagnostic. Core networking and other optional protocols retain
their existing platform support.

## Lifetime, close and errors

Run asynchronous operations on the owning `io_context`. The context must outlive
all transport handles; close/destroy handles on that reactor, or while it is not
running. Keep public objects stable while calling their asynchronous members.
Shared internal state keeps the configuration, parent connection and engine alive
while streams remain. Destroying a connection wrapper alone therefore does not
invalidate streams. The last shared handle releases its transport; queued data
must be acknowledged before releasing all handles if delivery matters.

`stream.reset(application_error)` abandons the stream and cancels its local
pending operations. `finish_write()` half-closes only its sending direction.
`connection.close(application_error)` attempts a CONNECTION_CLOSE immediately,
invalidates every stream on that connection, and wakes blocked operations. It is
best-effort; there is no graceful drain/retransmission guarantee.
`listener.close()` cancels accepts and invalidates every accepted connection using
that listener's UDP engine. Reactor `stop()` cancels all pending waits. Closing
one accepted connection leaves other connections on that listener operational.

Cancellation throws `system_error(operation_canceled)`, handshake deadlines use
`timed_out`, remote connection closure uses `connection_aborted`, and TLS/QUIC
protocol failures use `protocol_error` with OpenSSL diagnostics. Invalid API
arguments throw `invalid_argument`; operation overlap/state errors use
`logic_error`. Catch per-peer errors inside servers so they do not stop unrelated
root tasks.

This initial transport API does not expose 0-RTT, session resumption controls,
connection migration, QUIC DATAGRAM, custom congestion algorithms or
application-level graceful shutdown. It is not a replacement implementation of
QUIC cryptography: those protocol mechanisms remain in OpenSSL.

## Validation and references

`tests/quic_tests.cpp` uses local IPv4/IPv6 loopback: real TLS 1.3/QUIC handshakes,
certificate hostname/IP matching, untrusted/expired certificate rejection, ALPN
rejection, concurrent binary bidirectional streams, unidirectional streams in
both directions, FIN, handshake timeouts, cancellation, connection/listener close,
stream reset and overlapping-operation rejection. HTTP/3 has separate protocol
and interoperability tests; these are functional checks, not performance claims.

The event integration follows OpenSSL's
[event handling](https://docs.openssl.org/3.5/man3/SSL_handle_events/),
[event deadlines](https://docs.openssl.org/3.5/man3/SSL_get_event_timeout/),
[QUIC listener](https://docs.openssl.org/3.5/man3/SSL_new_listener/) and
[stream configuration](https://docs.openssl.org/3.5/man3/SSL_get_value_uint/) APIs.
