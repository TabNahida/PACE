# Performance results and tuning

[Documentation](README.md) · [Benchmark methodology](../benchmarks/README.md) · [中文阅读入口](README.zh-CN.md)

PACE's performance goal is to exceed Asio and libuv on useful workloads. The
measurements below do **not** establish that goal across platforms. This page
includes the complete backend comparison behind the README's selected bar chart,
component improvements and regressions, and links to every archived trial.

## Read the metrics

The current TCP runner records the following for every backend under the same
workload. Report all three backends together; a throughput lead alone does not
establish a latency or memory advantage.

| Metric | What it measures | How to read it |
|---|---|---|
| Throughput, requests/s | Verified complete request/response round trips divided by the measured wall-clock interval | Higher is better for this fixed workload; it does not measure connection setup or HTTP processing. |
| p50 latency, µs | Median client-observed round-trip latency among measured requests | Typical request; lower is better. |
| p95 / p99 latency, µs | Nearest-rank 95th / 99th percentile of the same measured requests | Tail latency; approximately 5% / 1% of samples are slower. Lower is better. |
| Maximum latency, µs | Slowest measured request in one trial | Useful for finding stalls; sensitive to trial length and host scheduling. |
| Peak process memory, MiB | OS process high-water mark, sampled after server/client shutdown and before result aggregation | Includes server, blocking clients, runtime, stacks and stored latency samples; it is **not the library's isolated footprint**. |

Tables report the median across independent trials for each metric. A median of
trial p99 values is not a pooled p99 across all requests. Likewise `median_max_us`
is the median of trial maxima; `max_max_us` in JSON is the worst request across
all trials. Min–max bars show the observed range, not a confidence interval.

Memory covers the **current executable image**, including startup and warmup;
it does not share throughput's measured-only time window. Linux reads
`/proc/self/status`'s `VmHWM`, macOS uses
`task_info(MACH_TASK_BASIC_INFO).resident_size_max`, and Windows uses
`GetProcessMemoryInfo().PeakWorkingSetSize`. Linux RSS accounting is approximate.
FreeBSD peak memory is explicitly unavailable: its process-history counter can
include pre-exec launcher memory, and a current-image provider has not been
verified. Its throughput and latency remain measured.

JSON preserves availability, source, scope and window. Compare memory only within
the same OS, toolchain and workload; OS accounting differs, and more requests
also allocate more latency samples. No steady-state allocation or isolated-server
claim is made. `--check-memory` verifies peak retention after allocation/free and
independence from a launcher with an extra 128 MiB. This runs separately from timing.

Historical reports below predate p95 and memory collection. Their missing values
remain unreported; they cannot be reconstructed from p50/p99 or throughput. The
current format uses `extended_metrics_version: 2` and requires every completed
request to contribute one latency sample. The initial version 1 POSIX memory
measurements were affected by inherited launcher peaks; the
[diagnostic trials](../benchmarks/results/local-metrics-diagnostic-2026-10-21/README.md)
are preserved and excluded from current memory comparisons.
[Exact timing boundaries and validation](../benchmarks/README.md).

中文说明：吞吐越高、同一负载下的延迟越低通常越好，但必须结合尾延迟一起看。
p95/p99 是单轮请求的延迟分位数，汇总表取各轮分位数的中位数；峰值内存包含
服务端、客户端和样本存储，且覆盖当前程序启动、预热和测量过程，不是 PACE 本身的
内存占用。FreeBSD 暂无已验证的当前程序峰值来源，明确显示不可用。历史报告未采集
的 p95 与内存不会补算；第一版受启动器影响的内存数据保留作诊断，不用于优势结论。

## Historical TCP comparison behind the README chart

[Successful CI run, 20 October 2026](https://github.com/TabNahida/PACE/actions/runs/39404014672).
Source head `72327e4a2f5b787deeb902d188e26c67f9799441`; checkout merge
`b1c994212888585ea06a71f7276463379314bd93`. Five rotating trials per backend;
**one server thread, four persistent connections, 64-byte messages, one outstanding
request per connection, 10,000 measured + 1,000 warmup requests per connection**.
Asio 1.34.2 and libuv 1.52.1. Both endpoints enable TCP_NODELAY and verify every
response byte. Startup, connection establishment and warmup are excluded.

| Platform | PACE req/s | Asio req/s | libuv req/s | Highest median |
|---|---:|---:|---:|---|
| [Linux / C++20](../benchmarks/results/ci-workers-2026-10-20/ubuntu-latest-cxx20.json) | 100,211 | 104,742 | 98,069 | Asio |
| [Linux / C++23](../benchmarks/results/ci-workers-2026-10-20/ubuntu-latest-cxx23.json) | 66,318 | 69,558 | 63,401 | Asio |
| [macOS arm64 / C++20](../benchmarks/results/ci-workers-2026-10-20/macos-latest-cxx20.json) | 50,116 | 44,079 | 51,678 | libuv |
| [Windows x64 / C++20](../benchmarks/results/ci-workers-2026-10-20/windows-latest-cxx20.json) | 34,120 | 35,607 | 36,560 | libuv |
| [FreeBSD 14.3 VM / C++20](../benchmarks/results/ci-workers-2026-10-20/freebsd.json) | 238,219 | 232,652 | 269,490 | libuv |

Values are medians, not best trials. Compare within rows only: different jobs use
different shared machines, including the two Linux language standards. A C++23
vs C++20 performance conclusion cannot be drawn from these rows.

The README selects **macOS PACE vs Asio (1.14× median throughput)** and **Linux
C++23 PACE vs libuv (1.05×)**. It does not plot the other comparator in each row.
Neither selection establishes that PACE is fastest on that platform, or that the
observed median lead is statistically reliable. The macOS min–max throughputs
were PACE 48,359–59,584 req/s and Asio 32,924–46,587 req/s in these five short
shared-runner trials. All raw trials, p50/p99 latency, spreads, environment metadata and hashes are preserved
in the [complete CI report](../benchmarks/results/ci-workers-2026-10-20/README.md).

The checked-in README chart preserves this historical selection. Its axis is
throughput relative to the named comparator's median, starting at zero. It does
not represent newer source changes or metrics that this snapshot did not collect.

For a **new** complete comparison, collect fresh extended-metric evidence and
render it explicitly (optional dependency: matplotlib):

```sh
xmake f -m release --sanitizers=n --profiling=n --benchmarks=y -y
xmake -y pace_bench
python3 scripts/run_benchmarks.py --require-all --repeats 5 --clients 4 \
  --requests 10000 --warmup 1000 --bytes 64 --output benchmarks/results/local
python3 scripts/render_performance_chart.py \
  --evidence benchmarks/results/local/report.json \
  --output /tmp/pace-performance.svg --preview /tmp/pace-performance.png
```

The runner writes the combined evidence to `report.json`. On Windows, use
`python`, put each command on one line, and choose writable output paths.
The renderer validates raw records and recomputes summaries before plotting all
three backends' throughput, p99 and peak process memory with trial ranges. It
rejects older records lacking those measurements. Keep this complete chart with
the full report; any selected README chart must name its workload and comparator
and link back to all results, including regressions.

## Earlier reactor optimization study

The [previous runtime study](../benchmarks/results/ci-runtime-2026-10-20/README.md)
measured a change that coalesces producer wakeups, skips an empty inbox
allocation/lock, and avoids unnecessary native polling when only runnable tasks
remain. Timers and newly submitted work are still checked each bounded turn.

Eight alternating paired runs used matching GCC C++20 `-O3 -DNDEBUG -pthread`
builds, the same five linked library sources, and no instrumentation. Component
costs are batch-average ns/op, not per-operation latency percentiles.

| Component | Local before ns/op | Local after ns/op | Earlier CI paired after/before |
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

### Remote worker measurements

The [successful four-platform CI study](../benchmarks/results/ci-workers-2026-10-20/README.md)
adds 180 scaling trials: three per cell for all three modes and 1/2/4/8 workers,
eight fixed clients, 5,000 measured + 500 warmup requests per client. All seven
jobs passed, including C++20/23 Linux builds, tutorials, plain/profile tests,
benchmark validation and sanitizers. The `run()` throughput medians below show
that the local eight-client result does not generalize to every host:

| Platform (reported logical CPUs) | 1 worker | 2 workers | 4 workers | 8 workers |
|---|---:|---:|---:|---:|
| Linux C++20 (4) | 96,939 | 172,882 | 190,415 | 191,350 |
| Linux C++23 (4) | 67,342 | 120,040 | 119,421 | 129,018 |
| macOS arm64 (3) | 66,948 | 60,976 | 70,215 | 52,936 |
| Windows x64 (4) | 36,427 | 53,096 | 74,367 | 86,430 |
| FreeBSD VM (4) | 259,126 | 273,775 | 268,238 | 257,059 |

Units are requests/s; compare within rows only. Eight workers beat four in the
Windows median, while macOS and FreeBSD favored smaller counts. Three trials
per cell are exploratory; all modes, spreads, lifecycle timings and p99 are in
the [unabridged reports](../benchmarks/results/ci-workers-2026-10-20/README.md).
This supports keeping count and execution placement configurable, without
equating workers to reported CPU count or promising a fixed optimum.

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
