# PACE

<p align="center">
  <img src="docs/assets/project-card.svg" alt="PACE — C++20 coroutine networking with independent worker reactors" width="100%">
</p>

<p align="center">
  <a href="https://github.com/TabNahida/PACE/actions/workflows/ci.yml"><img src="https://github.com/TabNahida/PACE/actions/workflows/ci.yml/badge.svg" alt="Build, test and compare"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-20%20%2F%2023-7c83ff" alt="C++20 and newer">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-63d7bf" alt="MIT license"></a>
</p>

<p align="center">English · <a href="README.zh-CN.md">简体中文</a> · <a href="docs/architecture.md">Architecture</a> · <a href="benchmarks/README.md">Benchmark methodology</a></p>

## Performance

**Surpassing libuv and Asio is a goal, not an established result.** PACE is an early, usable prototype. Its benchmark compares PACE and standalone Asio coroutines with libuv's native callback API under the same TCP echo workload.

<!-- benchmark-results:start -->
Measured in [CI on 20 October 2026](https://github.com/TabNahida/PACE/actions/runs/39397883915). Throughput in requests/s (median of five trials):

| Platform | PACE | Asio | libuv |
|---|---:|---:|---:|
| [Linux / C++20](benchmarks/results/ci-runtime-2026-10-20/ubuntu-latest-cxx20.json) | 101,093 | 103,577 | 99,196 |
| [Linux / C++23](benchmarks/results/ci-runtime-2026-10-20/ubuntu-latest-cxx23.json) | 61,553 | 64,620 | 56,866 |
| [macOS arm64 / C++20](benchmarks/results/ci-runtime-2026-10-20/macos-latest-cxx20.json) | 78,807 | 57,981 | 89,020 |
| [Windows x64 / C++20](benchmarks/results/ci-runtime-2026-10-20/windows-latest-cxx20.json) | 52,733 | 55,843 | 57,554 |
| [FreeBSD 14.3 VM / C++20](benchmarks/results/ci-runtime-2026-10-20/freebsd.json) | 355,542 | 344,665 | 408,762 |

Same workload: one server thread, four persistent connections, 64-byte messages, 10,000 measured plus 1,000 warmup requests per connection; Asio 1.34.2 and libuv 1.52.1. Compare within each row; platforms and runs use different shared machines. PACE does not yet outperform both baselines across platforms.

A separate Linux before/after experiment measured a **3.47% median paired TCP throughput increase** (8/8 wins; exploratory ratio interval 1.0288–1.0396). Local TCP pairs remain inconclusive. These findings do not establish a portable speedup.

[Raw comparisons, component regressions and stage diagnostics](benchmarks/results/ci-runtime-2026-10-20/README.md) · [Local TCP/profile evidence](benchmarks/results/tcp-profile-2026-10-20/README.md). All seven CI jobs passed, including plain/profile tests on the four platforms and sanitizers.
<!-- benchmark-results:end -->

A [local component study](benchmarks/results/component-2026-10-20/README.md) also records eight alternating before/after pairs per operation. The optimized build measured 58.26 ns per empty native poll, 18.88 ns per yield, 22.09 ns per same-context callback, and 22.89 ns per cross-thread enqueue in batches of 256 (median batch-average costs, Linux/WSL2, GCC 13.3). See the study for baseline values, exact units and uncertainty; these are component costs, not TCP or HTTP throughput.

Read the [methodology and reproduction commands](benchmarks/README.md). [GitHub Actions](https://github.com/TabNahida/PACE/actions/workflows/ci.yml) is configured to build, test, and upload comparison artifacts for Linux, macOS, Windows, and FreeBSD. Shared runners are noisy: compare backends within the same job, inspect repeated runs, and treat TCP echo results separately from HTTP speed or multicore scaling.

## Small API. Explicit ownership. Independent reactors.

PACE (Parallel Async Coroutine Engine) is a C++20 asynchronous networking library with coroutine tasks, timers, TCP, and HTTP/1.0–1.1 support. A fixed set of worker threads each owns an independent event loop. Coroutines stay on their assigned loop, keeping normal I/O execution local to one thread.

- **Choose your execution model.** Use `pace::runtime` for multiple worker reactors, or run a standalone `pace::io_context` on a thread you own.
- **Write ordinary control flow.** Compose lazy `pace::task<T>` operations with `co_await`; use RAII sockets, `std::span`, and `std::string_view`.
- **Serve HTTP directly.** Coroutine handlers return a `pace::http::response`; the library handles Content-Length framing and keep-alive.
- **Keep the core small.** Build with xmake and the platform socket API. Asio and libuv are optional benchmark dependencies, not library dependencies.

## A working HTTP server

```cpp
#include <pace/pace.hpp>

pace::task<pace::http::response> hello(pace::http::request request) {
    if (request.target == "/")
        co_return pace::http::response{.body = "Hello from PACE!\n"};
    co_return pace::http::response{.status = 404, .body = "Not found\n"};
}

int main() {
    pace::io_context io;
    pace::tcp::listener listener(io, {"127.0.0.1", 8080});
    io.spawn(pace::http::serve(listener, hello));
    io.run();
}
```

The [complete HTTP example](examples/http_server.cpp) adds worker selection and signal-driven shutdown. The [TCP echo example](examples/echo_server.cpp) shows `accept()`, `read_some()`, and `write_all()`.

TCP types live in `pace::tcp`: `listener`, `stream`, `endpoint`, and `listen_options`. Include `<pace/tcp.hpp>` for TCP or `<pace/pace.hpp>` for the complete library. Endpoints currently use numeric IPv4 addresses. The old `pace::tcp_listener`, `pace::tcp_stream`, `pace::ipv4_endpoint`, and `pace::listen_options` names remain compatibility aliases through `<pace/net.hpp>` and the umbrella header.

For multiple loops, construct `pace::runtime workers(4)`, assign work with `workers.context(index).spawn(...)`, and call `workers.run()`. The call starts and joins the worker threads; `workers.stop()` wakes every loop. Task placement is explicit; PACE currently does not migrate tasks or pin threads to CPU cores.

## Build and run

Install [xmake](https://xmake.io/guide/quick-start.html) 2.9.8 or newer and a C++20 compiler with standard coroutine support. CI uses xmake 3.0.9. Windows builds require the Visual Studio C++ toolchain. Normal builds need no third-party runtime dependencies.

On FreeBSD, install **xmake-io** from its upstream source or installer. The FreeBSD package named `xmake` is a different project; the CI workflow demonstrates a pinned source build.

```sh
git clone https://github.com/TabNahida/PACE.git
cd PACE
xmake f -m release -y
xmake -y
xmake test -v

# In separate terminal sessions:
xmake run http_server 8080 1
xmake run echo_server 9000 1
```

Visit `http://127.0.0.1:8080/`, or send a POST request to `/echo`. On platforms with `SO_REUSEPORT`, replace the final `1` with a worker count such as `4`. These server examples use one worker on Windows; the runtime itself supports multiple Windows worker loops.

```sh
# Select a newer language standard.
xmake f -m release --cxxstandard=cxx23 -y
xmake -y

# Linux/macOS/FreeBSD: enable AddressSanitizer + UndefinedBehaviorSanitizer.
xmake f -m debug --sanitizers=y -y
xmake -y
xmake test -v

# Build the optional comparison target, including Asio and libuv.
xmake f -m release --sanitizers=n --profiling=n --benchmarks=y -y
xmake -y pace_bench
python3 scripts/run_benchmarks.py --require-all --repeats 5 --clients 8 \
  --requests 5000 --warmup 500 --bytes 64 --output benchmarks/results/local
```

Use `python` instead of `python3` on Windows and enter the benchmark command on one line in PowerShell. The comparison runner finds the latest `pace_bench` binary under `build/`; pass `--executable` to select one explicitly. Its output includes Markdown and JSON summaries plus all raw measurements.

Available targets: `pace`, `pace_runtime_tests`, `pace_network_tests`, `http_server`, `echo_server`, and optional `pace_bench`. Enable the dependency-free `pace_components` target with `--components=y`; its [microbenchmark guide](benchmarks/components.md) explains operation boundaries and paired before/after runs. Build switches include `--tests=n` and `--examples=n`.

## Platform support

| Platform | Reactor and wakeup | CI configuration |
|---|---|---|
| Linux | `epoll` + `eventfd` | Native build, tests, benchmarks; C++20/23; sanitizer job |
| macOS | `kqueue` + `EVFILT_USER` | Native build, tests, benchmarks |
| Windows | `WSAPoll` + loopback socket | Native build, tests, benchmarks |
| FreeBSD | `kqueue` + `EVFILT_USER` | FreeBSD 14.3 VM build, tests, benchmarks |

The kqueue implementation targets 64-bit systems. Windows currently uses a functional `WSAPoll` baseline; an IOCP backend remains future work. `SO_REUSEPORT` is unavailable on Windows in this implementation. Platform support is implemented in source; inspect the current [workflow status and artifacts](https://github.com/TabNahida/PACE/actions/workflows/ci.yml) for actual validation results.

## Execution and lifetime rules

Each `io_context` runs on one thread. `spawn()`, `post()`, and `stop()` may be called from other threads; socket operations and close/cancel belong to the owning loop. One read and one write may be pending on a socket at the same time. Keep socket objects and buffers alive until their operations finish, and keep the context alive until its sockets are destroyed.

Tasks are lazy, move-only, and consumed once: use `co_await operation()` or `co_await std::move(task)`. Keep coroutine callable objects alive across suspension; named coroutine functions with values in their parameters are a straightforward default. Contexts run until explicitly stopped, including when idle, and are single-use. Stop them and join their running threads before destruction. See [architecture and lifecycle details](docs/architecture.md).

## Current scope

This is a foundation for development and measurement, not a production-hardened server. The HTTP layer supports HTTP/1.0 and HTTP/1.1, Content-Length bodies, persistent connections, and configurable header/body/request-count limits. It currently has **no request timeout**, so slow peers can occupy a connection indefinitely.

TLS, HTTP/2, chunked transfer encoding, streaming HTTP bodies, protocol upgrades, CONNECT tunnels, DNS resolution, IPv6, asynchronous file I/O, and per-operation cancellation/deadlines are not implemented. Numeric IPv4 endpoints are supported. CPU-heavy or blocking application work must run outside the reactor thread.

Near-term work is driven by measured bottlenecks: stronger cancellation and deadlines, broader protocol tests, native Windows completion I/O, and reproducible multiworker/HTTP load tests. Performance claims will stay tied to published evidence.

## License

[MIT](LICENSE) · Copyright © 2026 Tab Nahida.
