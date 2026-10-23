# Changelog

## Unreleased

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
