<p align="center">
  <img src="docs/assets/project-card.svg" alt="PACE — C++20 coroutine networking. Your threads. Your pace." width="100%">
</p>

<p align="center">
  <a href="https://github.com/TabNahida/PACE/actions/workflows/ci.yml"><img src="https://github.com/TabNahida/PACE/actions/workflows/ci.yml/badge.svg" alt="Build and test"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-70dfc3" alt="MIT license"></a>
</p>

<p align="center">English · <a href="README.zh-CN.md">简体中文</a> · <a href="docs/README.md">Documentation</a> · <a href="docs/reference/README.md">API</a></p>

**C++20 coroutine networking, from sockets to HTTP/3.** PACE (Parallel Async Coroutine Engine) runs on one thread, a fixed worker pool, or threads your application owns.

- **Write asynchronous code in sequence.** Await connections, reads, writes and timers with `co_await`.
- **Choose where work runs.** One reactor per worker, explicit task placement, caller-owned execution.
- **Build only what you use.** A core without third-party runtime dependencies, with optional protocol modules above it.

<p align="center">
  <a href="docs/performance.md"><img src="docs/assets/readme-performance.svg" alt="Local Linux TCP echo: PACE leads throughput and p50; Asio leads p95 and p99. Zero-based charts include all three libraries and trial ranges." width="100%"></a>
</p>

<p align="center"><a href="docs/performance.md">Full benchmark results, methodology &amp; raw data →</a></p>

## Four layers. Your stack.

| Layer | Modules | Aggregate target |
|:--|:--|:--|
| **3 · Applications** | [HTTP/1.1](docs/reference/http.md) · [HTTP/2](docs/reference/http2.md) · [HTTP/3](docs/reference/http3.md) · [WebSocket](docs/reference/websocket.md) | `pace_application` |
| **2 · Extensions** | [TLS](docs/reference/tls.md) · [DNS](docs/reference/dns.md) · [QUIC](docs/reference/quic.md) | `pace_extensions` |
| **1 · Networking** | [TCP](docs/reference/tcp.md) · [UDP](docs/reference/udp.md) · Socket API | `pace_net` |
| **0 · Runtime** | Event loop · Native polling · Tasks · Timers · Workers | `pace_runtime` |

Link individual modules, layer aggregates, or `pace` for the enabled stack. HTTP/1.1 and WebSocket are enabled by default; TLS, DNS, QUIC, HTTP/2 and HTTP/3 are opt-in. QUIC and HTTP/3 require POSIX; see the [platform matrix](docs/reference/platform.md).

[Architecture →](docs/architecture.md) · [Build options & integration →](docs/tutorial/01-build.md)

## A small HTTP server

```cpp
#include <pace/http.hpp>
#include <pace/run.hpp>

int main() {
    pace::http::router app;
    app.get("/", [](pace::http::request) {
        return pace::http::response{.body = "Hello, PACE!\n"};
    });
    pace::run([&](pace::io_context& io) {
        return pace::http::serve(io, {"127.0.0.1", 8080}, app);
    });
}
```

Link with `add_deps("pace_http1")`. [Write this server step by step →](docs/tutorial/04-http.md)

Continue step by step: [TCP](docs/tutorial/03-tcp.md) · [UDP](docs/tutorial/07-udp.md) · [DNS](docs/tutorial/08-dns-tls.md) · [QUIC](docs/tutorial/11-quic-http3.md) · [HTTP/3](docs/tutorial/11-quic-http3.md) · [WebSocket](docs/tutorial/09-websocket.md)

## Get started

You need [xmake](https://xmake.io/guide/quick-start.html) 2.9.8+ and a C++20 compiler.

```sh
git clone https://github.com/TabNahida/PACE.git
cd PACE
xmake f -m release -y
xmake -y tutorial_http
xmake run tutorial_http
```

The example starts a local server, verifies a request and exits. To keep it listening on port 8080, run `xmake run tutorial_http serve`.

<details>
<summary>Enable all protocols on Linux</summary>

```sh
xmake f -m release --tls=y --dns=y --quic=y --http2=y --http3=y -y
xmake -y
```

For a sockets-only build, disable the default application protocols with `--http1=n --websocket=n`. See the [build guide](docs/tutorial/01-build.md) for dependencies and per-module targets.

</details>

## Explore

| | |
|:--|:--|
| [Tutorials](docs/tutorial/README.md) | Tasks, protocols, workers and graceful shutdown |
| [API reference](docs/reference/README.md) | Signatures, ownership, errors and protocol limits |
| [Execution models](docs/tutorial/05-execution.md) | Calling thread, fixed workers and application-owned threads |
| [Platform support](docs/reference/platform.md) | Linux, macOS, FreeBSD and Windows backends |
| [Performance](docs/performance.md) | Local Linux measurements, tradeoffs and reproduction |

PACE is under active development; APIs and protocol coverage are evolving. See the [reference](docs/reference/README.md) for current limits.

[MIT](LICENSE) · © 2026 Tab Nahida
