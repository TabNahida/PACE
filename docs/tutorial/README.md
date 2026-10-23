# Learn PACE by running it

[中文学习指南](README.zh-CN.md) · [API reference](../reference/README.md)

## 1. Say hello

From the repository root:

```sh
xmake f -m release -y
xmake -y tutorial_tasks
xmake run tutorial_tasks
```

This prints `Hello, PACE` and exits. Start with [build and integration](01-build.md)
if you need a compiler or an application target. Read [tasks and timers](02-tasks-timers.md)
for `co_await`, lazy tasks, owned arguments and errors.

## 2. Pick a protocol

Run `xmake -y` to build examples. Every default command below is a complete,
bounded program, with source you can copy and adapt.

| Goal | Command | Guide / source |
|---|---|---|
| TCP echo | `xmake run tutorial_tcp` | [Guide](03-tcp.md) · [source](../../examples/tutorial_tcp.cpp) |
| DNS resolution | `xmake run tutorial_dns` | [API](../reference/dns.md) · [source](../../examples/tutorial_dns.cpp) |
| QUIC streams | `xmake run tutorial_quic` | [API](../reference/quic.md) · [source](../../examples/tutorial_quic.cpp) |
| HTTP/3 | `xmake run tutorial_http3` | [API](../reference/http3.md) · [source](../../examples/tutorial_http3.cpp) |
| UDP datagrams | `xmake run tutorial_udp` | [Guide](../reference/udp.md) · [source](../../examples/tutorial_udp.cpp) |
| HTTP server | `xmake run tutorial_http` | [Guide](04-http.md) · [source](../../examples/tutorial_http.cpp) |
| HTTP client | `xmake run tutorial_http_client` | [Guide](04-http.md) · [source](../../examples/tutorial_http_client.cpp) |
| TLS connection | `xmake run tutorial_tls` | [Guide](../reference/tls.md) · [source](../../examples/tutorial_tls.cpp) |
| HTTP/2 exchange | `xmake run tutorial_http2` | [Guide](../reference/http2.md) · [source](../../examples/tutorial_http2.cpp) |
| WebSocket messages | `xmake run tutorial_websocket` | [Guide](../reference/websocket.md) · [source](../../examples/tutorial_websocket.cpp) |

TLS, DNS, QUIC, HTTP/2 and HTTP/3 are optional. Before those examples, enable the features and rebuild:

```sh
xmake f -m release --tls=y --dns=y --quic=y --http2=y --http3=y -y
xmake -y
```

TLS examples load public test fixtures from `tests/certs` using the repository
root as their working directory. These certificates and keys are for tests only.
TCP and HTTP examples accept `serve` for a persistent server; stop it with Ctrl-C.
[HTTP](04-http.md) also explains HTTPS configuration and client connection reuse.

## 3. Choose execution and shutdown

```sh
xmake run tutorial_workers
xmake run tutorial_workers background
xmake run tutorial_shutdown
```

[Execution](05-execution.md) shows caller-owned loops, explicit worker counts and
both runtime modes. [Shutdown](06-shutdown.md) explains stopping acceptance,
draining connections, cancellation and joining. These examples check behavior;
use the [benchmark tools](../../benchmarks/README.md) to measure performance.

## 4. Look up your API

Use the [reference](../reference/README.md) for signatures, defaults, ownership,
thread rules and errors. Code fragments explain the complete linked programs;
read their source for include lists and error handling.
