# HTTP and WebSocket performance investigation

[TCP comparison](performance.md) · [CI architecture coverage](ci.md)

These measurements use local Linux. HTTP/1 and WebSocket compare PACE with its
own v0.1.1 behavior; the HTTP/2 and HTTP/3 experiment compares handler limits in
the same updated binary. The older TCP Asio/libuv comparison does not establish
HTTP or WebSocket performance rankings. Remote CI checks functionality and
portability.

## HTTP/1 response completion

[Issue #15](https://github.com/TabNahida/PACE/issues/15) identified an avoidable
wait after a fully framed `Connection: close` response. The client had already
parsed the complete message, but `send()` awaited graceful transport shutdown,
including the peer's FIN or TLS close notification, for up to 100 ms.

The default now attempts local shutdown with zero peer-wait budget and returns
the completed response. `client_options` can select an explicit `graceful`
policy and timeout. EOF-delimited responses still need EOF to establish their
message boundary; server-side admission rejection retains its bounded drain.

With an intentionally delayed peer close, ten trials per case gave these
median `send()` times (connect and TLS handshake excluded):

| Transport | Peer close delay | v0.1.1 | Updated default |
|---|---:|---:|---:|
| TCP | 50 ms | 50.1444 ms | 0.0187 ms |
| TLS | 50 ms | 50.2601 ms | 0.1098 ms |

For every updated closing-response sample with a 1, 10, 50 or 200 ms peer delay,
the client finished before the peer began closing. EOF-delimited responses
continued to wait for EOF, and the explicit graceful policy retained the wait.
This isolates teardown latency in a two-byte response; it is not an HTTP
throughput multiplier. Both endpoints set TCP_NODELAY. An initial diagnostic
without that setting is retained separately because delayed ACK/Nagle behavior
confounded its TLS timings.

[Complete HTTP data, configuration and build provenance](../benchmarks/results/http-close-linux-2026-10-23/README.md).

## WebSocket frame processing

The previous decoder read the prefix, optional length, mask and payload into
separate exact-size buffers. Short frames therefore required several receive
calls even when their bytes were already available. Each data frame also
allocated a temporary payload and copied it into the final message; outgoing
frames allocated fresh storage.

The updated implementation uses an 8 KiB read-ahead buffer, copies data fragments
directly into the final message, and reuses outgoing frame capacity. Larger
payloads can bypass the read-ahead buffer. Masking processes eight bytes at a
time using alignment-safe copies and a repeated key, with a byte-wise tail;
the implementation works in either byte order. Frame validation, masking randomness,
UTF-8 checks and limits retain their existing semantics. The tradeoff is an
additional 8 KiB per stream and retained send-buffer capacity; this is not a
claim of lower connection memory usage.

Median roundtrips per second from ten alternating trials per workload:

| Payload / fragmentation | v0.1.1 | Updated | Change |
|---|---:|---:|---:|
| 64 B / single frame | 256,431 | 330,083 | +28.7% |
| 16 KiB / single frame | 44,342 | 158,337 | +257.1% |
| 64 KiB / 1 KiB fragments | 3,339 | 6,252 | +87.2% |

[WebSocket measurements and reproducible commands](../benchmarks/results/websocket-linux-2026-10-23/README.md)
include ten alternating trials per fixed workload and separate syscall-count
diagnostics. Both peers share a single reactor and thread. Every binary message
has an incrementing sequence and its entire echoed payload is verified. TLS,
multi-worker scaling and other WebSocket libraries are outside this experiment.
The public stream still permits one asynchronous operation at a time.

## HTTP/2 and HTTP/3 handler concurrency

The bottleneck was in PACE's adapter: its protocol loop awaited each application
handler before processing the next completed request. A slow asynchronous
handler consequently delayed unrelated streams on the same connection.

The adapter now dispatches handlers up to `max_concurrent_handlers`, additionally
bounded by the stream limit. One driver owns the nghttp session and transport;
handlers place completed responses in a bounded queue and wake that driver.
This allows network progress and independent streams to continue while a handler
awaits I/O. It does not make CPU-bound handlers parallel on one reactor. Worker
placement remains the mechanism for parallel CPU execution.

In a controlled local Linux probe, each batch contains 16 requests whose handlers
each await a 10 ms timer. Ten batches per configuration, using the same updated
binary and excluding connect/handshake, produced these medians:

| Protocol | Handler limit 1 | Handler limit 8 |
|---|---:|---:|
| HTTP/2 over TCP | 163.036 ms | 20.500 ms |
| HTTP/2 over TLS | 163.167 ms | 20.847 ms |
| HTTP/3 | 164.040 ms | 21.016 ms |

All responses and observed handler peaks were validated. The approximately
eightfold improvement is the expected overlap of this synthetic asynchronous
work, not a general throughput result or a direct old/new binary comparison.
[Raw batches, environment and reproduction](../benchmarks/results/protocol-multiplex-linux-2026-10-23/README.md).

HTTP/2 uses nonblocking TCP/TLS progress with explicit readiness wakeups and
preserves TLS retry buffers. It also batches generated frames at a 64 KiB
threshold, potentially including one final frame. The TLS path reserves 128 KiB
of bounded ciphertext buffers per connection so simultaneous writes can keep
receiving under socket backpressure. HTTP/3 wakes its existing
QUIC event driver. Neither HTTP adapter adds a periodic polling timer; the QUIC
transport uses timerfd on Linux and retains its existing 1–10 ms timer fallback
on macOS/FreeBSD. Setting the handler limit to one retains sequential
application dispatch.

See the [HTTP/2](reference/http2.md) and [HTTP/3](reference/http3.md) references
for bounds, handler lifetime and cancellation contracts. Regression tests cover
overlapping handlers, stream limits, out-of-order completion, large responses,
TLS backpressure and completion after a disconnected peer.
