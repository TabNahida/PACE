# PACE documentation

Write your own asynchronous application, from its first main.cpp to HTTP/3.

Start with the [guided learning path](tutorial/README.md); chapters include complete
programs, incremental changes, expected output and ownership explanations.

[Project](../README.md) · [简体中文](README.zh-CN.md) · [API reference](reference/README.md)

## Start here

Create your own main.cpp and xmake.lua in [chapter 1](tutorial/01-build.md),
then add asynchronous tasks, TCP and HTTP. Each chapter gives build steps,
expected results and a small change to try yourself.

[Build](tutorial/01-build.md) → [Tasks](tutorial/02-tasks-timers.md) → [TCP](tutorial/03-tcp.md) → [HTTP](tutorial/04-http.md) → [Workers](tutorial/05-execution.md) → [Shutdown](tutorial/06-shutdown.md)

## Explore the layers

Choose only the modules your application needs. The core has no third-party dependencies.

| Layer | Guides | Runnable examples |
|---|---|---|
| **Runtime** | [Tasks](reference/task.md) · [Event loop](reference/io-context.md) · [Workers](reference/runtime.md) | [Tasks](../examples/tutorial_tasks.cpp) · [Workers](../examples/tutorial_workers.cpp) |
| **Networking** | [TCP](tutorial/03-tcp.md) · [UDP](tutorial/07-udp.md) | [TCP](../examples/tutorial_tcp.cpp) · [UDP](../examples/tutorial_udp.cpp) |
| **Base extensions** | [TLS](tutorial/08-dns-tls.md) · [DNS](tutorial/08-dns-tls.md) · [QUIC](tutorial/11-quic-http3.md) | [TLS](../examples/tutorial_tls.cpp) · [DNS](../examples/tutorial_dns.cpp) · [QUIC](../examples/tutorial_quic.cpp) |
| **Application protocols** | [HTTP/1.1](tutorial/04-http.md) · [HTTP/2](tutorial/10-http2.md) · [HTTP/3](tutorial/11-quic-http3.md) · [WebSocket](tutorial/09-websocket.md) | [HTTP](../examples/tutorial_http.cpp) · [HTTP/2](../examples/tutorial_http2.cpp) · [HTTP/3](../examples/tutorial_http3.cpp) · [WebSocket](../examples/tutorial_websocket.cpp) |

HTTP/1.1 and WebSocket are enabled by default. TLS, DNS, QUIC, HTTP/2 and HTTP/3
are opt-in; see the [build options](tutorial/01-build.md). TLS/QUIC examples use
public test-only certificates under `tests/certs`.

## Understand the design

- [Architecture](architecture.md) — layers, reactors, ownership and scheduling.
- [API reference](reference/README.md) — signatures, errors and concurrency contracts.
- [Tutorials](tutorial/README.md) — step-by-step application construction, expected results and companion checks.
- [Performance](performance.md) — full local Linux measurements, tradeoffs and reproduction.
- [Protocol performance](protocol-performance.md) — HTTP completion, WebSocket framing and concurrent HTTP/2/3 handlers.
- [CI coverage](ci.md) — native, emulated and compile-only architecture checks.
- [Platform support](reference/platform.md) — backends and supported configurations.

TCP, UDP and QUIC support numeric IPv4 and IPv6 endpoints; DNS resolves A and
AAAA records. HTTP/1 offers request-head admission, per-request limits and bounded
streaming responses. HTTP/2 and HTTP/3 bodies remain buffered. Protocol references
describe deadlines, ownership and individual limits.
