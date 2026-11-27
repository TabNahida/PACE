# PACE documentation

[简体中文](README.zh.md) · [Project](../README.md)

PACE is a C++20 coroutine networking library: a runtime of reactors and tasks, TCP and UDP, TLS, DNS, QUIC,
HTTP/1.1, HTTP/2, HTTP/3 and WebSocket. The documentation is organized by what you want to do.

## I want to…

| | Start here |
|:--|:--|
| …get something running in five minutes | [Install and write a first program](tutorial/01-install-and-first-program.md), then [an HTTP client](tutorial/02-http-client.md) |
| …learn PACE properly | The [tutorial](tutorial/README.md), in order: HTTP, tasks, TCP, UDP, structured concurrency, workers, shutdown, then each protocol |
| …solve a specific problem | The [guides](guides/README.md): HTTP configuration, protocol tuning |
| …look up a type or a function | The [reference](reference/README.md), one section per module |
| …understand how it works and why | The [explanation](explanation/README.md): architecture, execution model, ownership, API design |
| …see how fast it is | [Performance](performance/README.md), with methods and raw data |
| …upgrade from an earlier version | [Migration](migration/README.md) |
| …contribute | [Contributing](contributing/README.md): building, testing, style, documentation |

## Modules

| Module | Header | Reference | Tutorial |
|:--|:--|:--|:--|
| Runtime | `<pace/runtime.hpp>` | [runtime](reference/runtime/README.md) | [tasks and timers](tutorial/04-tasks-and-timers.md), [structured concurrency](tutorial/07-structured-concurrency.md), [workers](tutorial/08-workers-and-threads.md), [shutdown](tutorial/09-graceful-shutdown.md) |
| Networking | `<pace/net.hpp>` | [TCP and UDP](reference/net/README.md) | [TCP](tutorial/05-tcp.md), [UDP](tutorial/06-udp.md) |
| TLS, DNS | `<pace/tls.hpp>`, `<pace/dns.hpp>` | [TLS](reference/tls/README.md), [DNS](reference/dns/README.md) | [TLS and DNS](tutorial/10-tls-and-dns.md) |
| QUIC | `<pace/quic.hpp>` | [QUIC](reference/quic/README.md) | [QUIC and HTTP/3](tutorial/13-quic-and-http3.md) |
| HTTP | `<pace/http.hpp>` | [HTTP](reference/http/README.md) | [client](tutorial/02-http-client.md), [server](tutorial/03-http-server.md) |
| HTTP/1.1, HTTP/2, HTTP/3 | `<pace/http1.hpp>`, `<pace/http2.hpp>`, `<pace/http3.hpp>` | [HTTP/1.1](reference/http1/README.md), [HTTP/2](reference/http2/README.md), [HTTP/3](reference/http3/README.md) | [HTTP/2](tutorial/12-http2.md), [HTTP/3](tutorial/13-quic-and-http3.md) |
| WebSocket | `<pace/websocket.hpp>` | [WebSocket](reference/websocket/README.md) | [WebSocket](tutorial/11-websocket.md) |
| Gateway | `<pace/gateway.hpp>` | [gateway](reference/gateway/README.md) | [files and proxies](tutorial/14-gateway.md) |

[Build options](reference/build-options.md) · [Platform support](reference/platform-support.md) ·
[Errors](reference/errors.md)
