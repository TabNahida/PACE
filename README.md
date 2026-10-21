# PACE

<p align="center">
  <img src="docs/assets/project-card.svg" alt="PACE — C++20 coroutine networking with independent worker reactors" width="100%">
</p>

<p align="center">
  <a href="https://github.com/TabNahida/PACE/actions/workflows/ci.yml"><img src="https://github.com/TabNahida/PACE/actions/workflows/ci.yml/badge.svg" alt="Build, test and compare"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-20%20%2F%2023-7c83ff" alt="C++20 and newer">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-63d7bf" alt="MIT license"></a>
</p>

<p align="center">English · <a href="README.zh-CN.md">简体中文</a> · <a href="docs/README.md">Get started</a> · <a href="docs/reference/README.md">API reference</a> · <a href="docs/performance.md">Performance</a></p>

PACE (Parallel Async Coroutine Engine) is a C++20 coroutine networking library. Run tasks on a fixed set of independent worker reactors, on the calling thread, or on threads your application owns. Use `co_await` with TCP, UDP, HTTP/1.1, WebSocket, and optional TLS/HTTPS and HTTP/2.

## Selected performance advantages

<p align="center">
  <a href="docs/performance.md"><img src="docs/assets/selected-performance.svg" alt="Selected TCP echo medians: macOS throughput 17.7% higher than Asio; local Linux p95 30.5% lower than libuv; Windows whole-process peak memory 13.3% lower than libuv. Different hosts and workloads; whiskers show trial ranges." width="100%"></a>
</p>

Selected **21 October 2026 TCP echo medians** across different hosts and workloads. Memory includes clients and server. These pairwise results are not an overall ranking or an HTTP/TLS benchmark. [All three libraries, throughput, latency, memory, raw trials and methodology →](docs/performance.md)

## Run your first task

Install [xmake](https://xmake.io/guide/quick-start.html) 2.9.8+ and a compiler with C++20 coroutine support, then run:

```sh
git clone https://github.com/TabNahida/PACE.git
cd PACE
xmake f -m release -y
xmake -y tutorial_tasks
xmake run tutorial_tasks
```

This prints `Hello, PACE` and exits. The smallest complete application is:

```cpp
#include <pace/pace.hpp>
#include <iostream>

pace::task<void> hello(pace::io_context& io) {
    std::cout << "Hello, PACE\n";
    io.stop();
    co_return;
}

int main() {
    pace::io_context io;
    io.spawn(hello(io));
    io.run();
}
```

`io.run()` executes on the caller and stays alive until stopped. For multiple workers, use `pace::runtime workers(4)`, submit through `workers.context(index)`, and call `workers.run()`; `run_on_current_thread()` uses the caller plus three background threads. [Execution choices →](docs/tutorial/05-execution.md)

## Choose a protocol

| Task | Runnable example | Guide |
|---|---|---|
| TCP client and echo server | `xmake run tutorial_tcp` | [TCP](docs/tutorial/03-tcp.md) |
| UDP datagrams | `xmake run tutorial_udp` | [UDP](docs/reference/udp.md) |
| HTTP client / server | `xmake run tutorial_http_client` / `tutorial_http` | [HTTP](docs/tutorial/04-http.md) |
| TLS / HTTPS | `xmake run tutorial_tls` | [TLS](docs/reference/tls.md) · [HTTPS](docs/reference/http.md) |
| HTTP/2 requests | `xmake run tutorial_http2` | [HTTP/2](docs/reference/http2.md) |
| WebSocket messages | `xmake run tutorial_websocket` | [WebSocket](docs/reference/websocket.md) |

Build examples with `xmake -y`. TLS and HTTP/2 are optional: configure `xmake f -m release --tls=y --http2=y -y` first to resolve OpenSSL and nghttp2. The default core has no third-party runtime dependencies; Asio and libuv are only optional benchmark dependencies. [Build options and application integration →](docs/tutorial/01-build.md)

## Where to go next

- [Tutorials](docs/tutorial/README.md): tasks, protocols, workers and orderly shutdown.
- [API reference](docs/reference/README.md): signatures, ownership, errors and concurrency limits.
- [Performance and tuning](docs/performance.md): throughput, latency, process memory, worker scaling and reproduction commands.
- [Architecture](docs/architecture.md): Linux `epoll`, macOS/FreeBSD `kqueue`, Windows `WSAPoll`.

PACE is under active development and is not production-hardened. Endpoints currently use numeric IPv4; DNS, IPv6, asynchronous file I/O, streaming HTTP bodies and per-operation deadlines remain future work. Windows IOCP is not implemented. Keep sockets and borrowed buffers alive until operations finish, and keep blocking application work off reactor threads. See each protocol reference for its current limits.

[MIT](LICENSE) · Copyright © 2026 Tab Nahida.
