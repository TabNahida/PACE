# Build an application with PACE

Start with the [0.3.1 quick start](00-quick-start.md): install the package, run a small HTTP service, then add waits and a URL client.

[中文学习路线](README.zh-CN.md) · [API reference](../reference/README.md)

These chapters teach you to write your own files, build them, inspect the result,
and then change one behavior at a time. Complete source programs are companion
exercises, not a substitute for the explanation. The guides track this checkout;
component helpers require PACE v0.3.1 or newer. Use the package quick start or
the source setup in chapter 1.

## Follow the core path

| Chapter | What you will build and learn |
|---|---|
| [1. Your first project](01-build.md) | Create main.cpp and xmake.lua; link only the required module |
| [2. Tasks and timers](02-tasks-timers.md) | Return an asynchronous result; understand ownership, exceptions and automatic entry shutdown |
| [3. TCP](03-tcp.md) | Build separate echo server/client programs and handle byte-stream framing |
| [4. HTTP](04-http.md) | Add routes, POST bodies and async handlers, then a C++ client and HTTPS |
| [5. Execution](05-execution.md) | Choose two workers or application threads without hiding task placement |
| [6. Shutdown](06-shutdown.md) | Observe cancellation, stop acceptance, drain and destroy resources in order |

Start with chapter 1 rather than copying a server's setup wholesale. The application
commands run from your practice project; `tutorial_*` commands run from the PACE
repository root after configuring `--examples=y`. Persistent `serve` examples need
an explicit stop or Ctrl-C; the default companion invocations finish by themselves.

## Extend the application by protocol

| Chapter | What changes from the previous code | Companion |
|---|---|---|
| [7. UDP](07-udp.md) | Datagram boundaries, source endpoint and truncation | `tutorial_udp` |
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
limits and lifetime requirements. For the rationale behind the shorter entry and
routing forms, see [API ergonomics](../ergonomics.md).

Use [component conveniences](../reference/component-conveniences.md) for exact contracts and [protocol tuning](../reference/protocol-tuning.md) for buffer reuse, deadlines and concurrency.
