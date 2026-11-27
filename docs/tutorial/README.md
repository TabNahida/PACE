# Tutorial

[中文](README.zh.md) · [Documentation](../README.md) · [API reference](../reference/README.md)

The tutorial is one path through PACE, read in order. You start with a project of your own and an HTTP server, make
requests to it without any coroutine, and then learn tasks, sockets, concurrency and shutdown before moving on to
the optional protocols. Every chapter gives complete files, the commands to build them and the output to expect,
and ends with something that runs.

## Learning path

| Chapter | What you build and learn | Companion |
|:--|:--|:--|
| [1. Install PACE and build your first program](01-install-and-first-program.md) | Install xmake and a compiler, create `main.cpp` and `xmake.lua`, depend on one module, build and run; named modules | `tutorial_tasks` |
| [2. Make HTTP requests](02-http-client.md) | Direct `get`, `post` and `fetch`, a body, a timeout, HTTPS; when to use `async_*` | `tutorial_http_client` |
| [3. Build an HTTP server](03-http-server.md) | Routes, building responses, handlers that wait, awaiting another request | `tutorial_http` |
| [4. Tasks and timers](04-tasks-and-timers.md) | Return a result from a coroutine; ownership, exceptions and how `pace::run` ends | `tutorial_tasks` |
| [5. TCP](05-tcp.md) | `tcp::exchange`, an echo server, framing | `tutorial_tcp` |
| [6. UDP](06-udp.md) | Request and reply, send-only, an echo server | `tutorial_udp` |
| [7. Structured concurrency](07-structured-concurrency.md) | Task groups, a shared deadline, local cancellation | `tutorial_structured` |
| [8. Workers and threads](08-workers-and-threads.md) | Two event loops, application threads, submitting work across threads | `tutorial_workers` |
| [9. Orderly shutdown](09-graceful-shutdown.md) | Cancellation, stop accepting, drain, destroy in order | `tutorial_shutdown` |
| [10. TLS and DNS](10-tls-and-dns.md) | Resolve a name, verify a TLS peer, keep address and identity apart | `tutorial_dns`, `tutorial_tls` |
| [11. WebSocket](11-websocket.md) | Upgrade a TCP connection, exchange messages, close | `tutorial_websocket` |
| [12. HTTP/2](12-http2.md) | One GET, then a multiplexed batch | `tutorial_http2` |
| [13. QUIC and HTTP/3](13-quic-and-http3.md) | QUIC streams and FIN, then HTTP/3 on top | `tutorial_quic`, `tutorial_http3` |
| [14. Static files and a reverse proxy](14-gateway.md) | Mount a directory, proxy an API, caching, workers | |

Apart from the HTTPS section of chapter 2, chapters 1 to 9 use only what the default build contains. Your own programs live in the practice project of
chapter 1 and run from there. The companion programs are complete, self-checking versions of the exercises in PACE's
`examples/` directory; build them from the PACE checkout after configuring with `--examples=y`, and run them with
`xmake run <name>`. They finish by themselves; the servers that keep listening (`tutorial_http serve`,
`tutorial_tcp serve`, `tutorial_websocket serve`) stop with Ctrl-C.

## Optional modules

Chapters 10 to 13 need modules that are off by default. Add their options when you configure, both in your project
and in the PACE checkout for the companions; xmake downloads the third-party libraries the first time.

| Chapter | Options | Downloads |
|:--|:--|:--|
| 10. TLS and DNS | `--tls=y --dns=y` | OpenSSL, c-ares |
| 11. WebSocket | `--websocket=y` (on by default) | |
| 12. HTTP/2 | `--http2=y`, plus `--tls=y` for HTTPS | nghttp2 |
| 13. QUIC and HTTP/3 | `--quic=y` for QUIC only, `--http3=y` for both | OpenSSL, ngtcp2, nghttp3 |

QUIC and HTTP/3 need a POSIX platform. The TLS and QUIC exercises use the public test certificates in
`tests/certs`, which are for local practice only. DNS resolves the numeric `127.0.0.1` by default and needs an
external network only for a real hostname. [Build options](../reference/build-options.md) and
[platform support](../reference/platform-support.md) give the exact configurations.

## After the tutorial

Once a chapter's exercise works, its reference page gives the defaults, limits and lifetime rules. The
[guides](../guides/README.md) hold task-oriented recipes, such as [HTTP recipes](../guides/http-recipes.md) and
[protocol tuning](../guides/protocol-tuning.md). For the reasoning behind the short client, entry and routing forms,
read [API design](../explanation/api-design.md).
