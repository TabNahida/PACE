# Runtime and networking hot paths

[简体中文](runtime.zh.md) · [Performance](README.md)

What the scheduler, the reactor and the socket layer cost per operation, measured on PACE 0.6.1. These are the
1.0 regression anchors: every later phase compares against them on the same machine.

| | |
|:--|:--|
| Version | PACE 0.6.1 (`v0.6.1`) |
| Host | Apple M6, 12 logical CPUs, 16 GiB, macOS 27.2, on AC power, otherwise idle |
| Compiler | Apple clang 21.0.0, release build |
| Measured | 27 November 2026, 5 trials per workload |
| Raw data | [`benchmarks/results/baseline-0.6.1-macos-arm64/`](../../benchmarks/results/baseline-0.6.1-macos-arm64/README.md) |

All values are medians across trials, with the observed minimum and maximum. Nothing here is compared with another
library: these numbers only make sense against the same workload on the same machine.

## Scheduler components

`pace_components` times one kind of operation in repeated batches on one thread and reports the batch average per
operation. Loop and harness overhead are included; the costs are not additive estimates of a TCP request.

| Operation | What one operation is | ns/op | Min–max |
|:--|:--|--:|--:|
| `lazy_task` | Create and destroy one unstarted task through a non-inlined factory | 13.96 | 13.81–14.92 |
| `await_chain` | Await and destroy a synchronous chain of 4 nested task frames, no reactor | 64.05 | 62.39–64.54 |
| `yield` | One suspension and resumption through the ready queue and a reactor pass | 5.03 | 4.85–5.58 |
| `post` | Enqueue and run one small callback on the same context | 7.38 | 6.77–7.67 |
| `cross_thread_post` | One `post` from another thread to a running reactor, including the wake-up | 7.27 | 6.54–8.20 |
| `spawn_completion` | Create, spawn, run and destroy one child task | 18.32 | 17.54–19.15 |
| `zero_timer` | One `sleep_for(0ns)`: clock read, timer heap insert and expiry, resumption | 28.25 | 27.77–29.52 |
| `empty_poll` | One call into kqueue with a zero timeout and nothing registered | 12,094.58 | 12,034.22–12,121.30 |

`empty_poll` is the cost of the system call itself, which the run loop pays only when it has to block or check for
I/O; `yield` and `post` stay in user space while work is ready.

## Reactor and cancellation latency

`pace_runtime_hotpaths` measures single events end to end and reports latency percentiles across iterations.

| Workload | What is timed | p50 ns | p99 ns |
|:--|:--|--:|--:|
| cancel sparse, 0 background | Cancel one registered wait until its callback runs | 250 | 292 |
| cancel sparse, 65,536 background | The same, with 65,536 other registrations alive | 250 | 292 |
| readiness, 1 socket | One UDP socket becomes readable until the poller reports it | 1,750 | 5,125 |
| readiness, 256 sockets | 256 sockets become readable until the poller has reported all of them | 25,000 | 46,875 |
| inbox, 65,536 roots | 65,536 root tasks spawned before `run()` until all have run | 3,875 | 6,958 |

Cancellation cost does not depend on how many other registrations exist. A separate probe (`pace_inbox_allocations`)
counts heap allocations on the reactor thread while 10,000 callbacks are posted to it from another thread: 2 in
total, so the cross-thread submission path does not allocate in steady state.

## Socket layer

`pace_net_hotpaths` drives the poller and UDP sockets directly.

| Workload | What one operation is | ns/op | Min–max |
|:--|:--|--:|--:|
| poll-dense | One readiness event, 128 of 192 registered sockets ready per poll | 117.2 | 116.2–120.1 |
| poll-sparse | One readiness event, 1 of 192 registered sockets ready per poll | 2,292 | 1,791–2,417 |
| udp | One `udp::async_exchange` round trip on loopback, including filtering a datagram from another sender | 53,504 | 49,765–58,708 |

## HTTP routing

`pace_router_bench` resolves a request path against an exact-match route table.

| Routes | ns per lookup | Min–max |
|--:|--:|--:|
| 16 | 67.9 | 66.7–68.6 |
| 1,024 | 82.8 | 80.2–84.1 |

## Worker scaling

`pace_scaling` runs a PACE-only TCP echo with one listening port per worker and 8 fixed blocking clients (64-byte
messages, 2,000 measured requests per client). The three execution modes are the ways an application can run
reactors: a `runtime` pool, the calling thread joining as one worker (`caller`), or threads the application owns
(`external`).

| Workers | `runtime` req/s | `caller` req/s | `external` req/s | p50 µs (`runtime`) | p99 µs (`runtime`) |
|--:|--:|--:|--:|--:|--:|
| 1 | 227,144 | 226,760 | 226,054 | 31.62 | 87.79 |
| 2 | 259,619 | 263,376 | 262,697 | 28.96 | 58.96 |
| 4 | 237,828 | 237,731 | 237,711 | 31.88 | 60.33 |
| 8 | 212,234 | 211,696 | 215,138 | 36.42 | 64.17 |

The three modes cost the same. Throughput does not grow past two workers here because the 8 clients, the
coordinator and the server share one machine's cores: this measures the execution modes, not server capacity.
Starting the reactors takes 0.02–0.10 ms and shutting them down 0.13–0.18 ms for 1 to 8 workers, the same in every
mode ([lifecycle table](../../benchmarks/results/baseline-0.6.1-macos-arm64/scaling.md)).

## Reproduce

```sh
xmake f -c -m release -o build/baseline --tls=y --dns=y --quic=y --http2=y --http3=y \
    --benchmarks=y --components=y --scaling=y --protocol_benchmarks=y --tests=n --examples=n -y
xmake -y
python3 scripts/run_baseline.py --source . --build-dir build/baseline \
    --output benchmarks/results/<name> --label "<host description>" --repeats 5
```

Run on an otherwise idle machine; nothing pins CPUs or frequencies. On Apple Silicon a single process can land on
an efficiency core: compare two builds with many alternating runs (`scripts/compare_runtime_perf.py`), not one run
each.

## See also

- [TCP echo](tcp.md) — the same release against Asio and libuv
- [Execution model](../explanation/execution-model.md) — what the run loop does between these operations
- [`io_context` reference](../reference/runtime/io-context.md)
