# Build an application with PACE

[中文学习路线](README.zh-CN.md) · [API reference](../reference/README.md)

Start with [one-call clients](00-quick-start.md): write a direct HTTP GET, supply a
local service, build both and inspect the response. Then try TCP or UDP. Learn
coroutines when you need to compose async work or write a server.

These guides track the current checkout. Direct clients and the renamed HTTP
`async_` functions are unreleased; use the source setup, not the 0.3.1 package.
Each chapter includes complete files, build steps, expected output and a small
change to try. Companion programs provide independent checks.

## Follow the core path

| Chapter | What you will build and learn |
|---|---|
| [1. Your first project](01-build.md) | Create main.cpp and xmake.lua; link only the required module |
| [4. HTTP](04-http.md) | Direct GET/POST/fetch first, then routes, async handlers and HTTPS |
| [3. TCP](03-tcp.md) | Start with `tcp::exchange`, then build the echo server and learn framing |
| [7. UDP](07-udp.md) | Direct request/reply, send-only, then an async echo server |
| [2. Tasks and timers](02-tasks-timers.md) | Return an asynchronous result; understand ownership, exceptions and automatic entry shutdown |
| [12. Structured concurrency](12-structured-concurrency.md) | Concurrent children, local cancellation and a shared deadline |
| [5. Execution](05-execution.md) | Choose two workers or application threads without hiding task placement |
| [6. Shutdown](06-shutdown.md) | Observe cancellation, stop acceptance, drain and destroy resources in order |

Begin with the quick start, then choose the protocol you need. The application
commands run from your practice project; `tutorial_*` commands run from the PACE
repository root after configuring `--examples=y`. Persistent `serve` examples need
an explicit stop or Ctrl-C; the default companion invocations finish by themselves.

## Extend the application by protocol

| Chapter | What changes from the previous code | Companion |
|---|---|---|
| [8. DNS and TLS](08-dns-tls.md) | Resolve an address, retain hostname identity, verify handshake and close | `tutorial_dns`, `tutorial_tls` |
| [9. WebSocket](09-websocket.md) | Upgrade a TCP connection, exchange messages and close | `tutorial_websocket` |
| [10. HTTP/2](10-http2.md) | Make one GET, then move to multiplexed batches | `tutorial_http2` |
| [11. QUIC and HTTP/3](11-quic-http3.md) | End stream directions, configure ALPN, transfer protocol ownership | `tutorial_quic`, `tutorial_http3` |

Optional modules need their build flags, explained at the start of each chapter.
TLS/QUIC companions use public test-only certificates and local peers. DNS defaults
to numeric loopback and only needs external network when you supply a hostname.
QUIC/HTTP3 require POSIX. [Build options](../reference/build-configuration.md) and
[platform coverage](../reference/platform.md) describe the exact configurations.

When a chapter's experiment works, read the linked API contract for defaults,
limits and lifetime requirements. For the rationale behind the shorter client, entry and
routing forms, see [API ergonomics](../ergonomics.md).

Use [component conveniences](../reference/component-conveniences.md) for exact contracts and [protocol tuning](../reference/protocol-tuning.md) for buffer reuse, deadlines and concurrency.

[Direct clients and API migration](../reference/direct-clients.md)
