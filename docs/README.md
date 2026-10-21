# PACE documentation

[简体中文](README.zh-CN.md) · [API reference](reference/README.md)

## Run something first

From the repository root, with xmake 2.9.8+ and a C++20 compiler:

```sh
xmake f -m release -y
xmake -y tutorial_tasks
xmake run tutorial_tasks
```

The program prints `Hello, PACE` and exits. Its basic flow is
`io.spawn(hello(io)); io.run();`: the caller executes the task until it calls
`io.stop()`. [Build your own application](tutorial/01-build.md) or read the
[complete example](../examples/tutorial_tasks.cpp).

## Choose what to build

Build the default examples with `xmake -y`, then choose a path:

| I want to… | Start here | Complete source |
|---|---|---|
| Await tasks and timers | [Tasks](tutorial/02-tasks-timers.md) | [tutorial_tasks.cpp](../examples/tutorial_tasks.cpp) |
| Connect or listen with TCP | [TCP tutorial](tutorial/03-tcp.md) | [tutorial_tcp.cpp](../examples/tutorial_tcp.cpp) |
| Send UDP datagrams | [UDP reference](reference/udp.md) | [tutorial_udp.cpp](../examples/tutorial_udp.cpp) |
| Call or serve an HTTP API | [HTTP tutorial](tutorial/04-http.md) | [Client](../examples/tutorial_http_client.cpp) · [server](../examples/tutorial_http.cpp) |
| Encrypt connections / use HTTPS | [TLS](reference/tls.md) · [HTTPS](reference/http.md) | [tutorial_tls.cpp](../examples/tutorial_tls.cpp) |
| Exchange HTTP/2 requests | [HTTP/2 reference](reference/http2.md) | [tutorial_http2.cpp](../examples/tutorial_http2.cpp) |
| Exchange WebSocket messages | [WebSocket reference](reference/websocket.md) | [tutorial_websocket.cpp](../examples/tutorial_websocket.cpp) |
| Choose worker threads | [Execution](tutorial/05-execution.md) | [tutorial_workers.cpp](../examples/tutorial_workers.cpp) |
| Stop and release resources | [Shutdown](tutorial/06-shutdown.md) | [tutorial_shutdown.cpp](../examples/tutorial_shutdown.cpp) |

TLS and HTTP/2 require their optional build switches:
`xmake f -m release --tls=y --http2=y -y`, then `xmake -y`.
Default tutorial invocations finish automatically. TLS examples use public
**test-only** certificates under `tests/certs`; replace them for real services.

## Look up the details

The [API reference](reference/README.md) lists signatures, ownership, thread
requirements and errors. Begin with the protocol you use, then check shared
[task](reference/task.md), [context](reference/io-context.md) and
[runtime](reference/runtime.md) rules.

[Architecture](architecture.md) explains the reactor design.
[Performance and tuning](performance.md) includes all measured backends,
regressions, throughput, latency, process-memory interpretation and reproduction
links. The [tutorial index](tutorial/README.md) lists runnable commands.

PACE is under active development. Numeric IPv4 is supported; DNS, IPv6,
asynchronous file I/O, streaming HTTP bodies and per-operation deadlines are
not yet provided. Protocol support is bounded; consult each reference before
choosing it for an application.
