# Performance results and tuning

[Documentation](README.md) · [Benchmark methodology](../benchmarks/README.md) · [中文阅读入口](README.zh-CN.md)

PACE's performance goal is to exceed Asio and libuv on useful workloads. The
measurements below do **not** establish that goal across platforms. This page
includes the complete backend comparison behind the README's selected bar chart,
component improvements and regressions, and links to every archived trial.

## Complete TCP comparison behind the README chart

[Successful CI run, 20 October 2026](https://github.com/TabNahida/PACE/actions/runs/39397883915).
Source head `62c8724b177a9ca03ed0b607bc5a74a852c26753`; checkout merge
`0bc9df91ba4582e629ada6c5a77e7d62103d0804`. Five rotating trials per backend;
**one server thread, four persistent connections, 64-byte messages, one outstanding
request per connection, 10,000 measured + 1,000 warmup requests per connection**.
Asio 1.34.2 and libuv 1.52.1. Both endpoints enable TCP_NODELAY and verify every
response byte. Startup, connection establishment and warmup are excluded.

| Platform | PACE req/s | Asio req/s | libuv req/s | Highest median |
|---|---:|---:|---:|---|
| [Linux / C++20](../benchmarks/results/ci-runtime-2026-10-20/ubuntu-latest-cxx20.json) | 101,093 | 103,577 | 99,196 | Asio |
| [Linux / C++23](../benchmarks/results/ci-runtime-2026-10-20/ubuntu-latest-cxx23.json) | 61,553 | 64,620 | 56,866 | Asio |
| [macOS arm64 / C++20](../benchmarks/results/ci-runtime-2026-10-20/macos-latest-cxx20.json) | 78,807 | 57,981 | 89,020 | libuv |
| [Windows x64 / C++20](../benchmarks/results/ci-runtime-2026-10-20/windows-latest-cxx20.json) | 52,733 | 55,843 | 57,554 | libuv |
| [FreeBSD 14.3 VM / C++20](../benchmarks/results/ci-runtime-2026-10-20/freebsd.json) | 355,542 | 344,665 | 408,762 | libuv |

Values are medians, not best trials. Compare within rows only: different jobs use
different shared machines, including the two Linux language standards. A C++23
vs C++20 performance conclusion cannot be drawn from these rows.

The README selects **macOS PACE vs Asio (1.36× median throughput)** and **Linux
C++23 PACE vs libuv (1.08×)**. It does not plot the other comparator in each row.
Neither selection establishes that PACE is fastest on that platform, or that the
observed median lead is statistically reliable. For example, the macOS min–max
throughputs overlap: PACE 51,014–84,539 req/s; Asio 47,378–69,731 req/s. All raw
trials, p50/p99 latency, spreads, environment metadata and hashes are preserved
in the [complete CI report](../benchmarks/results/ci-runtime-2026-10-20/README.md).

Regenerate the README chart from those JSON files with
`python scripts/render_performance_chart.py` (optional dependency: matplotlib).
Its axis is throughput relative to the named comparator's median; each panel
uses a zero origin. It is an editorial selection of measured median leads.

## Reactor optimization study

The previous runtime change coalesces producer wakeups, skips an empty inbox
allocation/lock, and avoids unnecessary native polling when only runnable tasks
remain. Timers and newly submitted work are still checked each bounded turn.

Eight alternating paired runs used matching GCC C++20 `-O3 -DNDEBUG -pthread`
builds, the same five linked library sources, and no instrumentation. Component
costs are batch-average ns/op, not per-operation latency percentiles.

| Component | Local before ns/op | Local after ns/op | CI paired after/before |
|---|---:|---:|---:|
| Empty native poll | 74.60 | 58.26 | 0.8405 |
| Yield | 125.72 | 18.88 | 0.0838 |
| Same-context post | 129.11 | 22.09 | 0.0991 |
| Cross-thread enqueue, batch 256 | 325.27 | 22.89 | 0.0520 |
| Root task completion | 31.66 | 30.61 | 0.9562 |
| Zero-duration timer | 138.52 | 33.13 | 0.1495 |
| Lazy task | 5.70 | 5.69 | 0.9523 |
| Synchronous await chain, depth 4 | 35.84 | 35.86 | **1.0454** |

Lower favors the candidate for these components. Cross-thread enqueue excludes
batch-drain waits; it is **not** end-to-end completion latency. Exact boundaries,
compiler and environment are in the [component guide](../benchmarks/components.md)
and [local matched study](../benchmarks/results/component-2026-10-20/README.md).

The synchronous await chain regressed about **4.5% in CI**, despite unchanged task
headers; local matched measurements were unchanged. The cause is not established.
The CI TCP median paired ratio was **1.0347**, 8/8 wins, exploratory 95% interval
1.0288–1.0396. [Local TCP pairs](../benchmarks/results/tcp-profile-2026-10-20/README.md)
were inconclusive (ratio 1.0492, interval 0.9786–1.1440, 6/8 wins).

An [inbox reuse experiment](../benchmarks/inbox-reuse-experiment.md) reduced
allocation but regressed TCP throughput by 3.62% and was reverted. Smaller
component cost alone is not sufficient evidence for an end-to-end optimization.

## Where the time goes

[Optional stage instrumentation](../benchmarks/profiling.md) measures send, recv,
polling, registration, inbox, timers, callbacks and coroutine execution. Probes
cover server lifetime, including startup/warmup/shutdown, and add overhead.
Instrumented throughput is rejected by the fair comparison runner.

In the local after sample, send accounted for about 45.7% of observed wall time,
blocking poll 34.7%, recv 9.3%, exclusive coroutine execution 4.0%, and poller
updates 2.9%. Blocking poll includes idle time; it is not CPU overhead. The inbox
improved from about 83.9 to 16.7 ns/turn but initially represented only about
0.74% of observed time. [Raw stages and commands](../benchmarks/results/tcp-profile-2026-10-20/README.md)
and [all-platform stages](../benchmarks/results/ci-runtime-2026-10-20/README.md#stage-diagnostics)
show why scheduler speedups cannot be translated directly into TCP speedups.

## Worker count and execution placement

`runtime(n)` already accepts any positive number of workers. Hardware concurrency
is a default hint, not an affinity assignment or a requirement. Use a measured
worker budget that leaves capacity for clients, application work and the OS.

- One busy connection has one outstanding request in the current echo test;
  extra workers cannot make that connection parallel.
- More active contexts may help with enough independent connections, but also
  add threads, polling, wakeups and scheduling competition.
- `io_context::run()` supports application-owned threads. Socket work remains on
  its context; execute CPU-heavy or blocking application work elsewhere and post
  completion back to the owner. See the [execution tutorial](tutorial/README.md).
- Measure startup separately from steady-state throughput. Removing an idle
  coordinator thread or a thread creation does not prove faster socket I/O.

The [worker/execution study](../benchmarks/results/worker-scaling-2026-10-20/README.md)
measured 150 fresh-process trials locally: five per cell, three execution modes,
two fixed connection counts. The host reports 24 logical CPUs. Uninstrumented
GCC 13.3 C++20 release builds use 64-byte messages, 10,000 measured and 1,000 warmup
requests per client, with balanced connections on one port per worker.

| Workers | 8 clients: `run()` req/s | 48 clients: `run()` req/s |
|---|---:|---:|
| 1 | 303,599 | 314,390 |
| 2 | 276,037 | 579,760 |
| 4 | 228,556 | 813,434 |
| 8 | 210,373 | 1,086,909 |
| 24 | Not measured | 2,380,521 |
| 48 | Not measured | 1,931,579 |

These medians show why worker count is a workload choice. In the eight-client
sample, one worker had the highest throughput; with 48 clients, 24 workers had
the highest `run()` median, but a wide 1.12–2.61 million req/s range. Doubling to
48 workers did not improve that median. Do not divide the columns to estimate
scaling: changing client count changes the workload too.

The new `run_on_current_thread()` runs context zero on the caller and creates
N−1 background workers. For one worker in the eight-client study, median launch
cost fell from **50.1 µs to 1.9 µs** and shutdown from 154.9 µs to 76.9 µs.
Steady throughput was **1.8% lower** in that sample (297,995 vs 303,599 req/s);
other mode/worker cells also show no consistent caller-mode throughput win.
The API saves one thread and reduces small-runtime startup work; it is not
advertised as a universal TCP throughput optimization. Existing `run()` behavior
and its hardware-concurrency default are retained.

See [all modes, ranges, p99 and lifecycle measurements](../benchmarks/results/worker-scaling-2026-10-20/README.md)
and the [reproduction guide](../benchmarks/scaling.md). This is a separate
PACE-only workload; its numbers must not be compared directly against the
one-thread Asio/libuv table above.

## Reproduce and interpret

Use the [benchmark commands](../benchmarks/README.md),
[component runner](../benchmarks/components.md), and
[profiling guide](../benchmarks/profiling.md). All four platforms run correctness
tests and publish uninstrumented comparison artifacts through
[GitHub Actions](https://github.com/TabNahida/PACE/actions/workflows/ci.yml).

Correctness gates reject missing backends, malformed/incomplete results, wrong
request counts, corrupt payloads and timeout failures. They do not enforce an
arbitrary speed ratio on shared runners. For a release performance claim, use
dedicated hardware, predeclare all workload scenarios, run longer rotated trials
across sessions, and evaluate p99 alongside throughput. See the proposed
[acceptance criteria](../benchmarks/README.md#acceptance-criteria).

These closed-loop localhost tests do not establish HTTP performance, overload
latency or remote-network capacity. [Older CI results](../benchmarks/results/ci-2026-10-20/README.md)
remain available as historical evidence, not a controlled comparison across runs.
