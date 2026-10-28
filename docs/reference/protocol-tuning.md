# Protocol tuning and explicit control

[中文](protocol-tuning.zh-CN.md) · [Beginner tutorials](../tutorial/README.md) · [Convenience contracts](component-conveniences.md)

Start with the guided API, then measure the workload before changing these knobs.
Convenience wrappers preserve transport behavior; their owning allocations are
not performance improvements by themselves. Keep profiling separate from timing
comparisons; record payloads, connections, threads, feature flags and trial spread.
See [local benchmark methodology](../../benchmarks/README.md).

## TCP and UDP

Use `pace::write` and owning `pace::read_exact` when simple ownership matters.
For a hot path, retain a buffer and await `stream.write_all(string_view)` or
`read_some(span)` before reusing it. These APIs borrow; the buffer and stream must
remain alive and stable across suspension. Exact reads still need an application
framing rule and a maximum validated before allocating from an untrusted length.

`udp::socket::receive` allocates its capacity on each call (default 64 KiB).
For frequent datagrams use `receive_from(span)` with reused storage and inspect
`size`, `sender` and `truncated`. `send_to` avoids an owning string copy, but its
view must outlive the await. Neither form adds delivery guarantees or retries.

Do not overlap two reads or two writes on TCP, or two sends/two receives on UDP.
TCP's one-read-plus-one-write allowance does not apply to TLS or WebSocket.
`set_no_delay()` trades TCP coalescing for latency; measure it with your payload
and traffic pattern. IPv6 listeners are IPv6-only unless configured otherwise;
use port zero and `local_endpoint` for tests. Tune backlog explicitly through
`tcp::listen_options`, not by adding a new loop for each connection.
[TCP](tcp.md) · [UDP](udp.md) · [Cancellation](../tutorial/06-shutdown.md).

## DNS and TLS

The one-shot resolver owns one lookup. Keep an explicit `dns::resolver` to retain
its cancellation handle and reuse the wrapper for sequential lookups; do not infer
a persistent DNS cache or pooled channel from that reuse. One resolver supports
one active lookup. Options select numeric nameservers, tries and whole-lookup
expiry, not a connect deadline. Independent resolver instances can overlap waits.
System resolver/hosts setup can perform synchronous work; there is no helper
thread that makes every setup step asynchronous. [DNS details](dns.md).

Reuse a configured `tls::context` across connections rather than loading trust
roots for every default `tls::connect`. Configuration freezes on first stream
construction or SNI registration. Streams retain shared state, but lazy helpers
borrow custom config objects until their await completes. SNI routing and
application-only trust stores remain explicit. Configure ALPN `h2` for HTTP/2,
`http/1.1` for HTTP/1 when negotiated, and your own protocol for raw TLS.
Do not disable certificate verification to resolve mismatches. [TLS details](tls.md).

## WebSocket

`limits` bounds upgrade headers, frame bytes, message bytes, fragment count and
control frames consumed by a receive. `send_text`/`send_binary` own their payload;
`fragment_bytes = 0` uses the configured frame bound. Set a smaller fragment size
only for a workload or peer that needs it: excessive fragments add framing work.
`receive` reassembles a complete message and handles ping/pong within its bounds.

One active operation per stream includes sends, receives, ping/pong and close.
Do not use independent reader/writer tasks with this public API. Protocol drivers
have different internal rules; these do not grant application calls duplex access.
`close_gracefully` returns false if its budget expires and always closes transport;
bare EOF is not a successful WebSocket closing handshake. Use an authorization
callback for upgrade policy; the simple helper does not install one for you.
[Limits and closing contract](websocket.md).

## HTTP/1

A one-shot URL GET is an appropriate starting point. Use `http::client` or
`secure_client` for sequential keep-alive reuse; only one send may be active.
Advanced handlers can admit/reject a request after validating headers, set per-route
body limits, apply receive/write phase deadlines and stream a bounded response
producer. These are not global connection/memory limits or a preemptive timeout
for synchronous application code. The default final-response close policy does
not wait for the peer's FIN after complete framing. Opt into bounded graceful
closing only when its tradeoff is needed. [Recipes and configuration](http-recipes.md).

## HTTP2 and HTTP3

`get` owns a connection for one GET batch. To multiplex, submit multiple owned
requests to `http2::exchange` or `http3::exchange` on one connection:

```cpp
std::vector<pace::http::request> requests(2);
requests[0].method = "GET"; requests[0].target = "/one";
requests[1].method = "GET"; requests[1].target = "/two";
auto replies = co_await pace::http2::exchange(
    std::move(transport), "localhost", std::move(requests));
```

HTTP/3 uses the same shape with a verified QUIC connection. Results preserve input
order; response bodies remain buffered, with per-stream bounds. No connection
pool is created and these are not incremental streaming-body APIs.

A single driver owns the protocol session and transport. Application handlers
may suspend independently, with effective concurrency bounded by the minimum of
`max_concurrent_handlers` and `max_concurrent_streams` (both default to 100).
Set the handler cap to one for sequential dispatch. Increasing it can increase
memory and simultaneous downstream work; it does not add CPU threads. Use
explicit runtime workers for CPU parallelism, and synchronize shared handler state.
Normal disconnect does not forcibly destroy active handlers, so referenced state
must remain valid until they complete; stopping the context cancels PACE waits.

HTTP/2 over TLS requires verified ALPN `h2`; HTTP/3 requires `h3`. A mismatch fails
rather than falling back. Review [HTTP/2 flow control](http2.md) and
[HTTP/3 limits](http3.md) before changing transport windows or message bounds.

## QUIC

Configure trust and ALPN before the first connection. `quic::connect` defaults to
a 10-second handshake budget, independent of application request/stream deadlines.
`finish_write()` sends FIN for one stream direction; the peer can still reply.
Keep the connection alive while streams operate and the listener alive for its
accepted connections. `pace::write` / `write_all` accept bytes for transmission,
not peer acknowledgment. Before disposing a sender, await progress while
`pending_write_bytes()` is nonzero as in the [companion](../../examples/tutorial_quic.cpp).
Immediate `connection.close()` is best-effort, not delivery confirmation.

HTTP/3 owns its transferred QUIC connection, including control and QPACK streams;
do not also open application streams on it. This API does not expose 0-RTT,
WebTransport, CONNECT or server push. [QUIC details](quic.md).

## Worker placement

Choose worker counts by measuring the workload that matters, leaving CPU capacity
for clients, the kernel and any blocking workers. Keep payloads, connection counts,
compiler options and hardware placement consistent. The runtime provides neither
CPU affinity nor an adaptive pool. A count equal to logical CPUs is not evidence
of maximum throughput.

For inbound TCP/HTTP on platforms supporting reuse-port, the existing
[`echo_server`](../../examples/echo_server.cpp) and
[`http_server`](../../examples/http_server.cpp) examples create a listener per
context on the same port. The OS distributes newly accepted connections. Windows
does not currently support that strategy in PACE; those examples use one worker
there. Multiple contexts with separate ports or outbound connections are still
possible. Never move an active socket to another context as a balancing shortcut.


In the local QUIC exchange, the client also remains alive after reading the reply
FIN until the server has observed its acknowledgments. The companion awaits
connection events until both peers finish; receiving a FIN alone is not a join
with the remote sender.
