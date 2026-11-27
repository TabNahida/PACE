# PACE documentation

Start with [one-call clients](tutorial/02-http-client.md): get an HTTP response directly, then try TCP and UDP. These APIs require PACE 0.4.0 or newer.

Write your own asynchronous application, from its first main.cpp to HTTP/3.

Start with the [guided learning path](tutorial/README.md); chapters include complete
programs, incremental changes, expected output and ownership explanations.

[Project](../README.md) · [简体中文](README.zh.md) · [API reference](reference/README.md)

## Start here

Create your own main.cpp and xmake.lua in [chapter 1](tutorial/01-install-and-first-program.md),
then use direct HTTP, TCP and UDP clients. Add async tasks when needed. Each chapter gives build steps,
expected results and a small change to try yourself.

[Quick start](tutorial/02-http-client.md) → [HTTP](tutorial/03-http-server.md) / [TCP](tutorial/05-tcp.md) / [UDP](tutorial/06-udp.md) → [Tasks](tutorial/04-tasks-and-timers.md) → [Workers](tutorial/08-workers-and-threads.md) → [Shutdown](tutorial/09-graceful-shutdown.md)

## Explore the layers

Choose only the modules your application needs. The core has no third-party dependencies.

| Layer | Guides | Runnable examples |
|---|---|---|
| **Runtime** | [Tasks](reference/runtime/task.md) · [Event loop](reference/runtime/io-context.md) · [Workers](reference/runtime/runtime.md) | [Tasks](../examples/tutorial_tasks.cpp) · [Workers](../examples/tutorial_workers.cpp) |
| **Networking** | [TCP](tutorial/05-tcp.md) · [UDP](tutorial/06-udp.md) | [TCP](../examples/tutorial_tcp.cpp) · [UDP](../examples/tutorial_udp.cpp) |
| **Base extensions** | [TLS](tutorial/10-tls-and-dns.md) · [DNS](tutorial/10-tls-and-dns.md) · [QUIC](tutorial/13-quic-and-http3.md) | [TLS](../examples/tutorial_tls.cpp) · [DNS](../examples/tutorial_dns.cpp) · [QUIC](../examples/tutorial_quic.cpp) |
| **Application protocols** | [HTTP/1.1](tutorial/03-http-server.md) · [HTTP/2](tutorial/12-http2.md) · [HTTP/3](tutorial/13-quic-and-http3.md) · [WebSocket](tutorial/11-websocket.md) | [HTTP](../examples/tutorial_http.cpp) · [HTTP/2](../examples/tutorial_http2.cpp) · [HTTP/3](../examples/tutorial_http3.cpp) · [WebSocket](../examples/tutorial_websocket.cpp) |

HTTP/1.1 and WebSocket are enabled by default. TLS, DNS, QUIC, HTTP/2 and HTTP/3
are opt-in; see the [build options](tutorial/01-install-and-first-program.md). TLS/QUIC examples use
public test-only certificates under `tests/certs`.

## Understand the design

- [Architecture](explanation/architecture.md) — layers, reactors, ownership and scheduling.
- [API reference](reference/README.md) — signatures, errors and concurrency contracts.
- [Tutorials](tutorial/README.md) — step-by-step application construction, expected results and companion checks.
- [Performance](performance/README.md) — full local Linux measurements, tradeoffs and reproduction.
- [Protocol performance](performance/protocols.md) — HTTP completion, WebSocket framing and concurrent HTTP/2/3 handlers.
- [CI coverage](contributing/ci.md) — native, emulated and compile-only architecture checks.
- [Platform support](reference/platform-support.md) — backends and supported configurations.

TCP, UDP and QUIC support numeric IPv4 and IPv6 endpoints; DNS resolves A and
AAAA records. HTTP/1 offers request-head admission, per-request limits and bounded
streaming responses. HTTP/2 and HTTP/3 bodies remain buffered. Protocol references
describe deadlines, ownership and individual limits.

The component helpers added in v0.3.1 are documented in [component conveniences](reference/component-conveniences.md). Buffer reuse, concurrency and close budgets are in [protocol tuning](guides/protocol-tuning.md). Direct calls, timeout contracts and HTTP name migration are in [direct clients](reference/direct-clients.md).

[Build a file server and reverse proxy](tutorial/14-gateway.md) · [nginx comparison](performance/history/gateway-optimization-2026-11-03.md)
