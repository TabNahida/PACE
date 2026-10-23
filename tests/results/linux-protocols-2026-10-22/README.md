# Local Linux validation · 22 October 2026

GCC 13.3.0, C++20, WSL2 Linux x86_64. All network tests use loopback;
DNS uses a local nameserver, and TLS/QUIC use the repository's public test CA.
Dependencies: OpenSSL 3.5.7, c-ares 1.34.8, nghttp2 1.70.0, nghttp3 1.18.0.

| Configuration | Result | Evidence |
|---|---|---|
| All protocols and tutorials | 23/23 pass | [log](all-protocols.txt) |
| Core only, every optional protocol disabled | 8/8 pass | [log](core-only.txt) |
| HTTP3 only; HTTP1, HTTP2, WebSocket, DNS and TCP TLS disabled | 12/12 pass | [log](http3-only.txt) |
| Python benchmark/reporting regressions | 47/47 pass | [log](python.txt) |
| Runtime, TCP, DNS, QUIC and HTTP3 instrumented suites | ASan/UBSan/LeakSanitizer pass | [record](validation.json) |
| Layered benchmark/components/scaling binaries | build passed | [record](validation.json) |

After the full-protocol log, additional DNS tests passed for cancellation-time
reentry, destruction during cancellation, and cancellation while waiting for a
UDP-to-TCP fallback response. They also passed under all three sanitizers.
QUIC covers 13 groups, including four concurrent 192 KiB binary streams,
unidirectional streams in both directions, certificate/ALPN rejection, timeouts,
overlap rejection, reset, stop and per-connection isolation. HTTP3 covers 130
requests across stream-credit limits, 256 KiB binary bodies, trailers, independent
RFC 9114/QPACK peers, malformed responses, limits and failed-handshake isolation.

The source hashes in [validation.json](validation.json) identify the implementation
and tests. The benchmark's separate [validation record](../../../benchmarks/results/linux-optimization-2026-10-22/validation.json)
includes runtime/TCP sanitizer checks and exact baseline/final rebuilds.
Third-party static dependencies themselves were not rebuilt with sanitizers.

Reproduce the three build configurations with the commands in
[the build guide](../../../docs/tutorial/01-build.md). For HTTP3-only:

```sh
xmake f -c -m release --http1=n --websocket=n --tls=n --dns=n --quic=n --http2=n --http3=y -y
xmake -y
xmake test -v
```

The HTTP3-only directory contains only `libpace_runtime.a`, `libpace_net.a`,
`libpace_quic.a`, `libpace_http3.a`; the core-only directory contains only the first
two. QUIC is implied by HTTP3 even with `--quic=n`.

These are functional checks, not performance evidence. No remote CI or new
Windows/macOS/BSD validation was run. QUIC/HTTP3 explicitly reject Windows;
other POSIX fallback behavior remains unvalidated.
