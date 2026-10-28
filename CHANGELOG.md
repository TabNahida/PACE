# Changelog

## Unreleased

- Add structured task groups with owned factories, result handles, scope-owned resources and cancel-then-join error propagation; children cannot outlive the boundary.
- Add local cancellation sources/tokens, inherited task cancellation, shared monotonic deadlines and bounded cleanup shielding across runtime waits, DNS, QUIC and HTTP.
- Unify HTTP/1, HTTP/2 and HTTP/3 URL get/post/fetch and async_ forms; add http1 as an alias for http. **Breaking:** HTTP/2 and HTTP/3 transport get/exchange become async_get/async_exchange.
- **Lifetime change:** HTTP serve joins accepted connections on listener closure; HTTP/2 and HTTP/3 connections cancel and join active handlers before exiting.
- Add bilingual structured concurrency tutorials, complete concurrency contracts and unified-client migration guidance.

- Make HTTP URL `get`, `post` and `fetch` return responses directly without `pace::run`. **Breaking:** all former HTTP free-function coroutine overloads are now `async_get`, `async_post` and `async_fetch`; arguments stay unchanged.
- Add direct TCP/UDP `exchange`, async alternatives and send-only `udp::send`. Direct calls own their context on the calling thread, enforce a default cooperative 30-second total deadline and reject use inside a running PACE context.
- Teach the shortest direct clients first in English and Chinese; document exact contracts, tuning and the 0.3.1 migration in reference.

## 0.3.1 — 2026-10-28

- Add current-context TCP/QUIC listeners and UDP open/bind helpers, owned UDP packets, and shared owned write / exact-read helpers for TCP, TLS and QUIC.
- Add one-shot DNS lookup, verified TLS/QUIC connection helpers, WebSocket connect/upgrade/accept, and single-response HTTP/2 and HTTP/3 GET helpers.
- Teach the component helpers in English and Chinese with complete TCP, UDP, WebSocket, HTTP/2 and HTTP/3 programs; move HTTP recipes, build configuration and protocol tuning to reference.

## 0.3.0 — 2026-10-28

- Add no-argument and direct-task entry forms, current-context timers/yield, connected TCP streams and bounded exact reads.
- Add a small HTTP app, no-argument handlers, text/HTML/JSON response helpers, and URL GET/POST/fetch with optional asynchronous DNS and verified TLS.
- Add a package-based quick-start chapter in English and Chinese.
- Add `pace::run(factory)` for an owned, single-threaded coroutine entry with result and error propagation and background cancellation on completion.
- Add exact HTTP routing and owned adapters for synchronous/asynchronous handlers, plus an HTTP/1 serve overload that owns its listener.
- Replace the tutorial catalog with eleven progressive Markdown chapters in English and Chinese, including complete programs, expected results and lifetime explanations.

## 0.2.0 — 2026-10-23

- HTTP/1 clients complete fully framed responses without waiting for the peer's FIN; an explicit graceful close policy retains a configurable drain budget (#15).
- HTTP/2 and HTTP/3 dispatch bounded concurrent application handlers while a single driver owns each protocol session.
- HTTP/2 preserves completed requests after a TCP half-close and keeps TLS duplex transfers progressing under small socket buffers.
- WebSocket reads ahead into a bounded buffer, assembles data directly into the returned message, and reuses outgoing frame storage.
- Add cancellable combined read/write readiness waits for protocol drivers.
- Correct the Windows ARM64 OpenSSL build's stack-probe settings while retaining optimization, stack protection and all TLS algorithms.
- Expand remote checks to native 32/64-bit and ARM systems, Linux QEMU architectures including big-endian s390x, Android/iOS compilation, and MinGW.

## 0.1.1 — 2026-10-23

- TCP IPv6 endpoints, local/remote endpoint inspection, numeric interface scopes and explicit dual-stack listeners (#7). UDP, QUIC and HTTP/3 also support IPv6 endpoints.
- TLS server SNI certificate selection with independent fallback policies, immutable shared configuration, and application-supplied PEM trust bundles (#8).
- HTTP/1 request-head admission, per-request body limits, receive/write deadlines and owned, bounded streaming responses (#9).
- Cancellable callback timers release deadline state immediately; HTTP/1 phase deadlines and TCP/TLS/WebSocket close deadlines use scoped handles instead of polling or retaining sleeping tasks.
- README performance charts follow the introductory text in both languages; detailed protocol contracts and regression coverage accompany the new APIs.

## 0.1.0 — 2026-10-23

Initial PACE release: a C++20 coroutine networking library with independently
linkable runtime, networking, base extensions and application protocols.

- Runtime: per-worker reactors, tasks, timers, cross-thread submission and orderly shutdown.
- Networking: asynchronous TCP and UDP with Linux epoll, macOS/FreeBSD kqueue and Windows WSAPoll.
- Base extensions: optional TLS with OpenSSL, asynchronous DNS with c-ares and QUIC with OpenSSL 3.5.
- Application protocols: HTTP/1.1, HTTP/2, HTTP/3 and WebSocket clients and servers.
- Linux TCP optimization: persistent readiness interests and fewer redundant receive calls.
- Build: compact xmake options and separate library, test, example and benchmark modules.
- Documentation: runnable tutorials, protocol references and reproducible local Linux performance evidence.

QUIC and HTTP/3 require POSIX and are unavailable on Windows. Transport endpoints
currently use IPv4; DNS also resolves AAAA records. HTTP bodies are buffered.
HTTP/3 does not expose server push, CONNECT, WebTransport or 0-RTT. The 0.x API is
still evolving. See [platform support](docs/reference/platform.md), the protocol
references and [complete performance results](docs/performance.md).
