# Reference

[简体中文](README.zh.md) · [Documentation](../README.md)

Every public header under `include/pace/`, one section per module. Each module page follows the same order:
overview, synopsis, types, functions, errors, threads and concurrency, lifetime and ownership, limits and
defaults. Headers under `src/` are internal and not documented here.

| Module | Umbrella header | Target | Pages |
|:--|:--|:--|:--|
| Runtime | `<pace/runtime.hpp>` | `pace_runtime` | [overview](runtime/README.md) · [task](runtime/task.md) · [io_context](runtime/io-context.md) · [runtime](runtime/runtime.md) · [run](runtime/run.md) · [structured concurrency](runtime/structured-concurrency.md) · [I/O backends](runtime/io-backends.md) |
| Networking | `<pace/net.hpp>` | `pace_net` | [overview](net/README.md) · [TCP](net/tcp.md) · [UDP](net/udp.md) |
| TLS | `<pace/tls.hpp>` | `pace_tls` | [TLS](tls/README.md) |
| DNS | `<pace/dns.hpp>` | `pace_dns` | [DNS](dns/README.md) |
| QUIC | `<pace/quic.hpp>` | `pace_quic` | [QUIC](quic/README.md) |
| HTTP | `<pace/http.hpp>` | `pace_http` | [overview](http/README.md) · [routing](http/router.md) · [URL clients](http/clients.md) |
| HTTP/1.1 | `<pace/http1.hpp>` | `pace_http1` | [HTTP/1.1](http1/README.md) |
| HTTP/2 | `<pace/http2.hpp>` | `pace_http2` | [HTTP/2](http2/README.md) |
| HTTP/3 | `<pace/http3.hpp>` | `pace_http3` | [HTTP/3](http3/README.md) |
| WebSocket | `<pace/websocket.hpp>` | `pace_websocket` | [WebSocket](websocket/README.md) |
| Gateway | `<pace/gateway.hpp>` | in `pace_http1` | [gateway](gateway/README.md) |

`<pace/pace.hpp>` includes the umbrella header of every enabled module.

| Across modules | |
|:--|:--|
| [Errors](errors.md) | Exception types and error codes used everywhere |
| [Build options](build-options.md) | xmake options, targets, named modules, diagnostics |
| [Platform support](platform-support.md) | Systems, compilers, I/O backends, native handles |

## Rules every module shares

These hold for every API unless its page says otherwise; the [runtime overview](runtime/README.md) states them
precisely and [Explanation](../explanation/README.md) gives the reasons.

- **Ownership.** Tasks are lazy and consumed once. Parameters passed by value belong to the task; references,
  spans and string views are borrowed and must outlive it. A socket stays at a stable address until its operations
  finish, and the context outlives its sockets. [More](../explanation/ownership.md)
- **Threads.** One thread runs each context. Only `spawn`, `post` and `stop` may be called from other threads.
  [More](../explanation/execution-model.md)
- **Concurrency per object.** TCP allows one read and one write at a time; TLS, WebSocket and the HTTP clients
  allow one operation at a time. Each page states its limit.
- **Errors.** Exceptions, rethrown through `co_await`. [More](errors.md)
- **Cancellation.** `stop()` and cancellation tokens resume suspended operations with
  `std::errc::operation_canceled`; deadlines with `std::errc::timed_out`.
