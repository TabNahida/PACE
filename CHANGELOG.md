# Changelog

## 1.0.0-alpha.2 — unreleased

- **Behavior change:** `task_group::cancel()` no longer makes the scope throw. The `operation_canceled` it causes in
  the body and the children is the requested outcome: the scope joins them and returns the body's value (or
  returns, for a `void` body), so "first to finish wins" works as a group the winner cancels. Cancellation of the
  enclosing operation and other exceptions still propagate; a value-returning body cancelled before producing a
  value still throws `operation_canceled`.
- `task_group::join()` closes the group as soon as it is called, even when it then throws because the group is
  already cancelled.
- Implementation helpers that were left in public namespaces when `detail` was removed are gathered into
  `entry_access`, `operation_access`, `group_access` and `pace::http::handler_access`. They were never part of the
  API; code that named `invoke_entry`, `scoped_operation`, `call_handler` or similar must stop doing so.

## 1.0.0-alpha.1 — 2026-11-27

First pre-release on the way to 1.0: the library is reorganized by module, and the documentation is rewritten.
Behavior and signatures are unchanged; include paths, xmake targets and the named-module interface changed. See
[migrating from 0.6 to 1.0](docs/migration/0.6-to-1.0.md).

- **Breaking:** every module has its own directory under `include/pace/<module>/`, its umbrella header
  `<pace/<module>.hpp>` and its own static library target. `<pace/http.hpp>` is now the protocol-independent HTTP
  module (messages and routing); HTTP/1.1 is `<pace/http1.hpp>`. `<pace/networking.hpp>`, `<pace/extensions.hpp>`,
  `<pace/application.hpp>` and the `pace_extensions` / `pace_application` targets are removed.
- **Breaking:** the `pace::detail` namespace and `include/pace/detail/` are gone; internal headers live in `src/`.
- **Breaking:** one C++20 named module per library module (`pace.runtime`, `pace.net`, `pace.http`, `pace.http1`,
  `pace.gateway`, `pace.tls`, `pace.dns`, `pace.quic`, `pace.http2`, `pace.http3`, `pace.websocket`); each exports
  only its own names. `import pace;` still imports everything enabled.
- Add `pace::ascii` (`<pace/runtime/ascii.hpp>`) for case-insensitive ASCII comparison.
- Delete copy assignment from `response_cache`, `quic::listener` and `cancellation_registration`, which copied
  shared internals.
- Document every public declaration with `///` comments.
- Rewrite the documentation in English and Chinese: a 14-chapter tutorial in reading order, guides, a reference
  page per module in one template, design explanations, performance reports with their raw data, and a
  [documentation site](https://tabnahida.github.io/PACE/). The README opens with measurements generated from the
  0.6.1 baseline.
- Record a [0.6.1 performance baseline](benchmarks/results/baseline-0.6.1-macos-arm64/README.md) that later
  pre-releases are compared against.
- Switch the tests to doctest with a per-test watchdog; add libFuzzer targets for HTTP/1 and WebSocket parsing,
  external interoperability runs (curl, OpenSSL, h2spec), clang-tidy on changed lines, per-module and
  header-self-containment builds, and documentation checks in CI.

## 0.6.1 — 2026-11-26

- Run TLS over owned asynchronous byte streams, including QUIC bidirectional streams, in-memory transports and nested TLS. Replay pre-read bytes and exchange close notifications under backpressure (#33, #35).
- Add client verification by leaf certificate or SPKI SHA-256 pin, an application verification callback and explicit insecure mode. Set the SNI name independently of the verified peer name (#34).
- Inspect the server-side ClientHello before certificate selection. Select a context or hand the original transport and pre-read bytes back without sending a TLS response (#36).
- Harden socket readiness and bound reactor dispatch work. Preserve timeout reasons during reactor shutdown (#23, #24).

## 0.6.0 — 2026-11-18

- Use ngtcp2 by default when QUIC or HTTP/3 is enabled. Select the native OpenSSL transport with `--quic_backend=openssl`.
- Reduce root coroutine scheduling, timer cancellation and Linux socket setup costs. Task-group and HTTP waiters borrow state from their owning coroutine. See the [runtime and TCP measurements](benchmarks/results/runtime-network-2026-11-18/README.md) for component costs and multi-worker throughput.
- Add the optional C++20 named module and ngtcp2 QUIC backend. Drive QUIC deadlines and continuations through the PACE reactor.
- Reduce routing and cross-thread submission overhead. Publish [QUIC throughput, latency and memory measurements](benchmarks/results/quic-backends-2026-11-18/README.md).
- Publish [split-process TCP long-connection profiles](benchmarks/results/tcp-long-connections-2026-11-18/README.md) and add worker affinity and pipelining controls to the scaling benchmark.

## 0.5.0 — 2026-11-03

- Reduce HTTP/1 header allocation and bypass body-reader coroutines for complete buffered messages; preserve framing, body limits, pipelining and cancellation.
- Speed up static content validators (portable XXH64), file traversal, date formatting and HTTP/1 encoding; remove redundant body copies, buffer clearing and a proxy coroutine wrapper.
- Add reactor-local single-snapshot cache reads with mutation/TTL invalidation, isolated reader counters and borrowed context-service lookup to reduce hot-key contention and reference-count traffic.

- Add HTTP directory mounts, bounded static responses, validators and byte ranges.
- Add reverse proxies with per-reactor keep-alive pools, round-robin/least-connections balancing, deadlines and bounded shared response caching.
- Add sharded CLOCK caches and dense generation-checked reactor descriptor slots.
- Add selectable Linux io_uring and Windows IOCP socket-notification backends; retain native fallbacks.
- Add bilingual gateway tutorials, backend reference and reproducible local Linux nginx comparisons. Restore the README PACE/Asio/libuv comparison and publish a full report linking both library and nginx gateway measurements, including regressions.

## 0.4.0 — 2026-10-28

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
still evolving. See [platform support](docs/reference/platform-support.md), the protocol
references and [complete performance results](docs/performance/README.md).
