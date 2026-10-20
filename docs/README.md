# PACE documentation

PACE is a C++20 coroutine networking library with independent reactor contexts,
an optional worker runtime, numeric IPv4 TCP, and a bounded HTTP/1 server layer.
Start with a runnable example, then use the reference to check ownership and
shutdown requirements. [中文学习指南](tutorial/README.zh-CN.md).

## Learn by building

1. [Build and run](tutorial/01-build.md): xmake, dependencies, standards and examples.
2. [Tasks and timers](tutorial/02-tasks-timers.md): lazy tasks, owned arguments and errors.
3. [TCP echo](tutorial/03-tcp.md): a client, server and checked round trip.
4. [HTTP services](tutorial/04-http.md): handlers, requests, responses and limits.
5. [Execution and worker selection](tutorial/05-execution.md): explicit worker counts and application-owned contexts.
6. [Orderly shutdown](tutorial/06-shutdown.md): stop accepting, drain, cancel and join.

Each tutorial links to a complete `examples/tutorial_*.cpp` source. Default
invocations finish by themselves and can be used as smoke checks; TCP and HTTP
examples also expose long-running server modes for interactive use.

## API reference

The [reference index](reference/README.md) maps all public types and functions to
their signatures, defaults, thread requirements, lifetimes and errors.

| Area | Reference |
|---|---|
| Coroutine ownership and awaiting | [`task<T>` / `task<void>`](reference/task.md) |
| Reactor, submission, readiness and timers | [`io_context`](reference/io-context.md) |
| Worker runtime | [`runtime`](reference/runtime.md) |
| TCP addresses, connections and listeners | [`pace::tcp`](reference/tcp.md) |
| HTTP requests, responses and serving | [`pace::http`](reference/http.md) |
| Headers, socket handles and old names | [Platform and compatibility](reference/platform.md) |

## Design and measurement

- [Architecture](architecture.md): scheduling, platform backends and cancellation.
- [TCP comparison methodology](../benchmarks/README.md): timing boundaries and fair comparisons.
- [Component measurements](../benchmarks/components.md): operation-specific costs.
- [Stage profiling](../benchmarks/profiling.md): diagnostic wall-clock accounting.

The documented protocol scope is deliberate: no TLS, DNS resolver, IPv6,
asynchronous file API, HTTP client, chunked transfer, HTTP/2, WebSocket or request
deadline API is currently provided. A TCP byte stream can carry a manually
constructed HTTP request, as in the tutorial, but that is not a general HTTP client.

[Complete performance results and tuning](performance.md) · [Worker/execution scaling](../benchmarks/scaling.md)
