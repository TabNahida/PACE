# Changelog

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
