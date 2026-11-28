# Protocol tuning and explicit control

[简体中文](protocol-tuning.zh.md) · [Guides](README.md) · [Tutorial](../tutorial/README.md) · [API reference](../reference/README.md)

Start with the simple API, then measure the workload before changing these knobs.
Owning convenience calls keep the transport's behavior; the extra allocations they
make for ownership are not performance improvements by themselves. Keep profiling
separate from timing comparisons, and record payloads, connections, threads, build
options and the spread across trials.
See the [local benchmark methodology](../../benchmarks/README.md).

## TCP and UDP

Use `pace::write` and the owning `pace::read_exact` when simple ownership matters.
For a hot path, keep a buffer and await `stream.write_all(pace::as_bytes(text))` or
`read_some(pace::as_writable_bytes(buffer))` before reusing it. The views cost nothing: they
reinterpret the same storage without copying. These calls borrow: the buffer and the stream must
stay alive and at the same address across suspension. Exact reads still need an application
framing rule, and a maximum checked before allocating from an untrusted length.

`udp::socket::receive` allocates its capacity on each call (64 KiB by default).
For frequent datagrams use `receive_from(span)` with reused storage and inspect
`size`, `sender` and `truncated`. `send_to` avoids an owning string copy, but the
bytes it views must outlive the await. Neither form adds delivery guarantees or retries.

Do not overlap two reads or two writes on TCP, or two sends or two receives on UDP.
TCP's one-read-plus-one-write allowance does not apply to TLS or WebSocket.
`set_no_delay()` trades TCP coalescing for latency; measure it with your payload
and traffic pattern. IPv6 listeners are IPv6-only unless configured otherwise;
use port zero and `local_endpoint()` in tests. Tune the backlog explicitly through
`tcp::listen_options`, not by adding a new loop for each connection.
[TCP](../reference/net/tcp.md) · [UDP](../reference/net/udp.md) · [Cancellation](../tutorial/09-graceful-shutdown.md).

## DNS and TLS

The one-shot `dns::resolve` owns one lookup. Keep an explicit `dns::resolver` to retain
its `cancel()` handle and reuse the object for sequential lookups; that reuse is not a
persistent DNS cache or a pooled channel. One resolver supports one active lookup;
independent resolver instances can wait at the same time. `dns::options` selects numeric
nameservers, tries and the whole-lookup expiry (5 seconds and 2 tries by default), not a
connect deadline. Reading the system resolver and hosts configuration can be synchronous;
no helper thread makes every setup step asynchronous. [DNS reference](../reference/dns/README.md).

Reuse a configured `tls::context` across connections rather than loading trust
roots for every default `tls::connect`. Configuration freezes on first stream
construction, on registration as an SNI target or on selection by a ClientHello
callback. Streams retain shared state, but lazy helpers borrow a custom configuration
until their await completes. SNI routing and application-only trust stores stay
explicit. Configure ALPN `h2` for HTTP/2, `http/1.1` for HTTP/1 when negotiated, and
your own protocol for raw TLS. Do not disable certificate verification to work around
name mismatches. [TLS reference](../reference/tls/README.md).

## WebSocket

`websocket::limits` bounds upgrade headers, frame bytes, message bytes, fragment count and
control frames consumed by one receive. `send_text` and `send_binary` own their payload;
`fragment_bytes = 0` uses `max_frame_bytes`. Set a smaller fragment size only for a
workload or peer that needs it: many fragments add framing work.
`receive` reassembles a complete message and handles ping/pong within its bounds.

One active operation per stream covers sends, receives, ping/pong and close.
Do not run independent reader and writer tasks on the public API. Protocol drivers
have different internal rules; they do not give application calls duplex access.
`close_gracefully` returns false if its budget expires and always closes the transport;
a bare EOF is not a successful WebSocket closing handshake. Use an authorization
callback for upgrade policy; the simple helper does not install one for you.
[WebSocket reference](../reference/websocket/README.md).

## HTTP/1

A one-shot URL GET is a good starting point. Use `http::client` or
`secure_client` for sequential keep-alive reuse; only one send may be active.
Handlers returning `server_response` can admit or reject a request after its headers
are validated, choose per-route body limits, apply receive and write deadlines and
stream a bounded response producer. These are not global connection or memory limits,
nor a preemptive timeout for synchronous application code. The default final-response
close policy does not wait for the peer's FIN after complete framing; opt into bounded
graceful closing only when you need that trade-off. [HTTP recipes](http-recipes.md).

## HTTP/2 and HTTP/3

`async_get` owns a connection for one GET. To multiplex, submit several owned
requests to `http2::async_exchange` or `http3::async_exchange` on one connection
(`<pace/http2.hpp>`, target `pace_http2`, `--http2=y`):

```cpp
std::vector<pace::http::request> requests(2);
requests[0].method = "GET"; requests[0].target = "/one";
requests[1].method = "GET"; requests[1].target = "/two";
auto replies = co_await pace::http2::async_exchange(
    std::move(transport), "localhost", std::move(requests));
```

`transport` is a connected `tcp::stream` (prior knowledge) or a verified `tls::stream`.
HTTP/3 uses the same shape with a verified QUIC connection. Results keep the input
order; response bodies stay buffered, with per-stream bounds. No connection
pool is created, and these are not incremental streaming-body APIs.

A single driver owns the protocol session and transport. Application handlers
may suspend independently; effective concurrency is the minimum of
`max_concurrent_handlers` and `max_concurrent_streams` (both 100 by default for HTTP/2,
both 32 for HTTP/3). Set the handler cap to one for sequential dispatch. Raising it can
increase memory and simultaneous downstream work; it does not add CPU threads. Use
explicit runtime workers for CPU parallelism, and synchronize shared handler state.
Connection exit cancels and joins active handlers before returning; closing a
listener cancels and joins accepted connections. Borrowed references must live until that
boundary. See [structured concurrency](../reference/runtime/structured-concurrency.md)
for local cancellation and shielding.

HTTP/2 over TLS requires verified ALPN `h2`; HTTP/3 requires `h3`. A mismatch fails
rather than falling back. Read the [HTTP/2](../reference/http2/README.md) and
[HTTP/3](../reference/http3/README.md) references before changing flow-control windows or message bounds.

## QUIC

Configure trust and ALPN before the first connection. `quic::connect` defaults to
a 10-second handshake budget, independent of application request or stream deadlines.
`finish_write()` (or `co_await shutdown_write()`, its `pace::byte_stream` form) sends FIN for one stream
direction; the peer can still reply.
Keep the connection alive while streams operate, and the listener alive for its
accepted connections. `pace::write` / `write_all` hand bytes to the transport;
they do not mean the peer acknowledged them. Before disposing of a sender, keep awaiting
progress while `pending_write_bytes()` is nonzero, as the
[QUIC example](../../examples/tutorial_quic.cpp) does.
An immediate `connection.close()` is best effort, not delivery confirmation.

In the local QUIC exchange, the client also stays alive after reading the reply
FIN until the server has observed its acknowledgments. The example awaits
connection events until both peers finish; receiving a FIN alone is not a join
with the remote sender.

HTTP/3 owns the QUIC connection handed to it, including the control and QPACK streams;
do not also open application streams on it. The API does not expose 0-RTT,
WebTransport, CONNECT or server push. [QUIC reference](../reference/quic/README.md).

## Worker placement

Choose worker counts by measuring the workload that matters, leaving CPU capacity
for clients, the kernel and any blocking threads. Keep payloads, connection counts,
compiler options and hardware placement consistent. The runtime provides neither
CPU affinity nor an adaptive pool. A count equal to the number of logical CPUs is not
evidence of maximum throughput.

For inbound TCP and HTTP on platforms with reuse-port, the
[`echo_server`](../../examples/echo_server.cpp) and
[`http_server`](../../examples/http_server.cpp) examples create one listener per
context on the same port with `tcp::listen_options{.reuse_port = true}`, and the
operating system distributes new connections. PACE does not support that strategy
on Windows, so those examples use one worker there; several contexts with separate
ports or outbound connections still work. Never move an active socket to another
context to balance load. See [workers and threads](../tutorial/08-workers-and-threads.md).
