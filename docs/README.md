# PACE documentation

Build asynchronous network applications with C++20 coroutines.

[Project](../README.md) · [简体中文](README.zh-CN.md) · [API reference](reference/README.md)

## Start here

```sh
xmake f -m release -y
xmake -y tutorial_tcp
xmake run tutorial_tcp
```

The example starts a local TCP server and client, exchanges a message, then exits.
Requires xmake 2.9.8+ and a C++20 compiler.

[Build and integrate](tutorial/01-build.md) → [Tasks and timers](tutorial/02-tasks-timers.md) → [TCP](tutorial/03-tcp.md) → [Workers](tutorial/05-execution.md) → [Shutdown](tutorial/06-shutdown.md)

## Explore the layers

Choose only the modules your application needs. The core has no third-party dependencies.

| Layer | Guides | Runnable examples |
|---|---|---|
| **Runtime** | [Tasks](reference/task.md) · [Event loop](reference/io-context.md) · [Workers](reference/runtime.md) | [Tasks](../examples/tutorial_tasks.cpp) · [Workers](../examples/tutorial_workers.cpp) |
| **Networking** | [TCP](tutorial/03-tcp.md) · [UDP](reference/udp.md) | [TCP](../examples/tutorial_tcp.cpp) · [UDP](../examples/tutorial_udp.cpp) |
| **Base extensions** | [TLS](reference/tls.md) · [DNS](reference/dns.md) · [QUIC](reference/quic.md) | [TLS](../examples/tutorial_tls.cpp) · [DNS](../examples/tutorial_dns.cpp) · [QUIC](../examples/tutorial_quic.cpp) |
| **Application protocols** | [HTTP/1.1](reference/http.md) · [HTTP/2](reference/http2.md) · [HTTP/3](reference/http3.md) · [WebSocket](reference/websocket.md) | [HTTP](../examples/tutorial_http.cpp) · [HTTP/2](../examples/tutorial_http2.cpp) · [HTTP/3](../examples/tutorial_http3.cpp) · [WebSocket](../examples/tutorial_websocket.cpp) |

HTTP/1.1 and WebSocket are enabled by default. TLS, DNS, QUIC, HTTP/2 and HTTP/3
are opt-in; see the [build options](tutorial/01-build.md). TLS/QUIC examples use
public test-only certificates under `tests/certs`.

## Understand the design

- [Architecture](architecture.md) — layers, reactors, ownership and scheduling.
- [API reference](reference/README.md) — signatures, errors and concurrency contracts.
- [Tutorials](tutorial/README.md) — complete commands and executable examples.
- [Performance](performance.md) — full local Linux measurements, tradeoffs and reproduction.
- [Protocol performance](protocol-performance.md) — HTTP completion, WebSocket framing and concurrent HTTP/2/3 handlers.
- [CI coverage](ci.md) — native, emulated and compile-only architecture checks.
- [Platform support](reference/platform.md) — backends and supported configurations.

TCP, UDP and QUIC support numeric IPv4 and IPv6 endpoints; DNS resolves A and
AAAA records. HTTP/1 offers request-head admission, per-request limits and bounded
streaming responses. HTTP/2 and HTTP/3 bodies remain buffered. Protocol references
describe deadlines, ownership and individual limits.
