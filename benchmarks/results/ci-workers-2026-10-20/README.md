# CI performance evidence — 20 October 2026

[Actions run 39404014672](https://github.com/TabNahida/PACE/actions/runs/39404014672)
passed all seven jobs: five platform/language configurations, sanitizers, and
Linux before/after comparisons. This snapshot contains successful measurements,
including results where PACE is slower. It does not establish general performance
leadership over Asio or libuv.

## Single-server-thread comparison

Five trials per backend; one server thread, four persistent connections, 64-byte
messages, pipeline=1, 10,000 measured plus 1,000 warmup requests per connection.
PACE and standalone Asio use coroutine APIs; libuv uses callbacks. Builds are
uninstrumented release builds, using Asio 1.34.2 and libuv 1.52.1. Throughput is
requests/s. Latencies are medians of each trial's p50/p99, in microseconds; they
are not pooled percentiles.

| Platform / raw evidence | Backend | Median req/s | Min–max req/s | p50 µs | p99 µs |
|---|---|---:|---:|---:|---:|
| [Linux / C++20](ubuntu-latest-cxx20.json) | pace | 100,211 | 95,819–100,774 | 39.62 | 49.60 |
| [Linux / C++20](ubuntu-latest-cxx20.json) | asio | 104,742 | 103,845–106,109 | 37.89 | 47.34 |
| [Linux / C++20](ubuntu-latest-cxx20.json) | libuv | 98,069 | 95,374–100,282 | 29.21 | 67.77 |
| [Linux / C++23](ubuntu-latest-cxx23.json) | pace | 66,318 | 65,775–67,398 | 58.00 | 83.29 |
| [Linux / C++23](ubuntu-latest-cxx23.json) | asio | 69,558 | 69,491–70,163 | 55.62 | 77.02 |
| [Linux / C++23](ubuntu-latest-cxx23.json) | libuv | 63,401 | 61,211–63,679 | 57.51 | 107.87 |
| [macOS arm64 / C++20](macos-latest-cxx20.json) | pace | 50,116 | 48,359–59,584 | 76.38 | 226.79 |
| [macOS arm64 / C++20](macos-latest-cxx20.json) | asio | 44,079 | 32,924–46,587 | 82.17 | 260.00 |
| [macOS arm64 / C++20](macos-latest-cxx20.json) | libuv | 51,678 | 44,251–56,100 | 70.96 | 246.58 |
| [Windows x64 / C++20](windows-latest-cxx20.json) | pace | 34,120 | 26,654–35,736 | 115.30 | 298.90 |
| [Windows x64 / C++20](windows-latest-cxx20.json) | asio | 35,607 | 25,128–37,766 | 109.70 | 186.00 |
| [Windows x64 / C++20](windows-latest-cxx20.json) | libuv | 36,560 | 35,033–38,518 | 103.60 | 295.50 |
| [FreeBSD 14.3 VM / C++20](freebsd.json) | pace | 238,219 | 235,294–239,662 | 15.02 | 54.48 |
| [FreeBSD 14.3 VM / C++20](freebsd.json) | asio | 232,652 | 227,326–237,613 | 15.08 | 54.79 |
| [FreeBSD 14.3 VM / C++20](freebsd.json) | libuv | 269,490 | 265,805–274,836 | 13.09 | 54.24 |

PACE's median throughput exceeds libuv on the two Linux jobs, and exceeds Asio
on macOS and FreeBSD. It does not lead both baselines in any of these five
configurations. In particular, the Linux C++20 and C++23 jobs ran on different
CPU models: their values do not isolate the effect of language standard.
Shared-runner variation is visible in the ranges, especially on macOS and
Windows. Compare backends within a platform job, and retain latency as well as
throughput when judging the tradeoff. [Full comparison methodology](../../README.md).

## Worker count and execution mode

A separate PACE-only workload holds the total at eight clients, 64-byte messages,
5,000 measured plus 500 warmup requests/client. Each worker listens on its own
port, with clients distributed evenly. Every mode/worker cell has three
fresh-process trials; cell order rotates. These are median requests/s:

| Platform / full summary | Mode | 1 worker | 2 workers | 4 workers | 8 workers |
|---|---|---:|---:|---:|---:|
| [Linux / C++20](scaling-ubuntu-latest-cxx20.md) | runtime | 96,939 | 172,882 | 190,415 | 191,350 |
| [Linux / C++20](scaling-ubuntu-latest-cxx20.md) | caller | 104,649 | 171,591 | 186,069 | 190,707 |
| [Linux / C++20](scaling-ubuntu-latest-cxx20.md) | external | 102,552 | 204,479 | 193,497 | 190,131 |
| [Linux / C++23](scaling-ubuntu-latest-cxx23.md) | runtime | 67,342 | 120,040 | 119,421 | 129,018 |
| [Linux / C++23](scaling-ubuntu-latest-cxx23.md) | caller | 67,102 | 113,836 | 115,897 | 121,919 |
| [Linux / C++23](scaling-ubuntu-latest-cxx23.md) | external | 66,992 | 126,932 | 117,192 | 131,540 |
| [macOS arm64 / C++20](scaling-macos-latest-cxx20.md) | runtime | 66,948 | 60,976 | 70,215 | 52,936 |
| [macOS arm64 / C++20](scaling-macos-latest-cxx20.md) | caller | 61,658 | 66,072 | 73,670 | 52,635 |
| [macOS arm64 / C++20](scaling-macos-latest-cxx20.md) | external | 61,568 | 66,941 | 67,411 | 51,144 |
| [Windows x64 / C++20](scaling-windows-latest-cxx20.md) | runtime | 36,427 | 53,096 | 74,367 | 86,430 |
| [Windows x64 / C++20](scaling-windows-latest-cxx20.md) | caller | 37,162 | 52,303 | 77,276 | 84,094 |
| [Windows x64 / C++20](scaling-windows-latest-cxx20.md) | external | 37,128 | 53,056 | 77,644 | 73,874 |
| [FreeBSD 14.3 VM / C++20](scaling-freebsd.md) | runtime | 259,126 | 273,775 | 268,238 | 257,059 |
| [FreeBSD 14.3 VM / C++20](scaling-freebsd.md) | caller | 259,016 | 274,296 | 265,059 | 259,189 |
| [FreeBSD 14.3 VM / C++20](scaling-freebsd.md) | external | 259,547 | 273,068 | 266,653 | 252,738 |

`runtime` starts N reactor threads and waits. `caller` starts N−1 threads and
runs context 0 on the calling thread. `external` uses N application-owned
`io_context` threads. All modes also have the same coordinator and blocking
client threads. Worker counts are explicit; eight workers exceed the reported
logical CPU count on every runner here.

There is no consistent steady-state winner among execution modes. For example,
macOS falls from its four-worker median to its eight-worker median in all three
modes, while FreeBSD largely plateaus around two workers. Linux and Windows gain
throughput with additional workers in this fixed-client workload, without linear
scaling. At one worker, caller mode has lower median startup time in all five
configurations, consistent with avoiding a spawned reactor thread; that does not
imply a universal steady-state throughput advantage.

The linked summaries retain throughput spread, p50/p99 and separate setup,
launch, client/warmup and shutdown measurements. `startup_ns` is setup + launch,
excluding coordinator creation between those intervals; steady-state throughput
excludes startup and warmup. Clients and server compete on the same machine,
and changing worker count changes connections per reactor. These results do not
measure shared-listener balancing, isolated server capacity or HTTP performance.
See [exact timing boundaries and reproduction commands](../../scaling.md).

## Paired before/after study

The Linux paired job compares baseline
[`0c42dcc9`](baseline-revision.txt) with candidate merge
[`b1c99421`](candidate-revision.txt). Both use GCC 13.3.0, C++20 and
`-O3 -DNDEBUG -pthread`, compiling the full core source set. Each case has eight
pairs with alternating before/after and after/before order. The component builds
use the same candidate benchmark harness; TCP uses the harness from each
revision. Compiler commands and fixed references are in the
[workflow](../../../.github/workflows/ci.yml).

**This comparison includes the earlier inbox, wakeup and polling optimizations.
It does not isolate the new caller-thread API.** The TCP comparison path runs a
standalone `io_context` and does not call `runtime::run_on_current_thread()`.

[Paired TCP evidence](paired-tcp.json): four clients, 64 bytes, 100,000 measured
plus 1,000 warmup requests/client. Throughput medians were **60,715 before** and
**62,898 after**. The median paired after/before ratio was **1.0357** (+3.57%),
with an exploratory 95% bootstrap interval of **1.0220–1.0459**; the candidate won
8/8 pairs. This job used an AMD EPYC 7763 runner, separate from the Linux C++20
backend-comparison job.

Components use 1,000,000 measured operations after 10,000 warmup operations,
chain depth 4 and batch size 256. Times below are medians of batch-average ns/op.
The ratio is the median of paired after/before ratios, not necessarily the ratio
of the two timing medians. **Lower is better for component costs**; positive
change is a regression. The whole four-frame await chain is one operation.

| Component / raw evidence | Before ns/op | After ns/op | Paired ratio | Cost change | Exploratory 95% interval | Candidate wins |
|---|---:|---:|---:|---:|---:|---:|---:|
| [`lazy_task`](paired-lazy_task.json) | 16.97 | 16.70 | 0.9818 | -1.82% | 0.9715–0.9907 | 8/8 |
| [`await_chain`](paired-await_chain.json) | 95.37 | 96.99 | 1.0074 | +0.74% | 0.9664–1.0217 | 3/8 |
| [`empty_poll`](paired-empty_poll.json) | 306.90 | 257.06 | 0.8424 | -15.76% | 0.8343–0.8483 | 8/8 |
| [`yield`](paired-yield.json) | 410.39 | 35.59 | 0.0867 | -91.33% | 0.0864–0.0878 | 8/8 |
| [`post`](paired-post.json) | 419.83 | 43.05 | 0.1024 | -89.76% | 0.0992–0.1034 | 8/8 |
| [`cross_thread_post`](paired-cross_thread_post.json) | 630.51 | 31.29 | 0.0500 | -95.00% | 0.0483–0.0729 | 8/8 |
| [`spawn_completion`](paired-spawn_completion.json) | 73.96 | 71.67 | 0.9696 | -3.04% | 0.9601–0.9761 | 8/8 |
| [`zero_timer`](paired-zero_timer.json) | 456.62 | 67.84 | 0.1489 | -85.11% | 0.1480–0.1505 | 8/8 |

The await-chain median paired cost increases by 0.74%; its interval crosses 1,
so this small regression is not evidence of a stable direction. The largest
reductions occur in task-only scheduling and batched producer enqueue. The
cross-thread row excludes batch-drain waits and is not callback completion
latency. Component costs do not add up to a TCP request's latency. See the
[exact operation definitions](../../components.md).

Intervals resample whole paired ratios 10,000 times with seed 0. Eight pairs on
one shared runner offer exploratory uncertainty estimates: scheduling drift,
frequency changes and correlated observations can violate the independence
assumption. Passing the job requires correct, complete measurements and does not
require a gain. Small changes in the unchanged task microbenchmarks can reflect
layout or environment effects; this combined-revision experiment does not
attribute a change to one implementation detail.

## Component and profiling evidence

The per-platform component suites have five rotated trials for each of eight
operations, with 100,000 measured operations and 5,000 warmup operations per
process. These differ in duration and machine from the paired study. Profiling
uses three instrumented PACE-only trials per platform, with four clients and
10,000 measured plus 1,000 warmup requests/client.

| Platform | Worker/execution scaling | Components | Profiling |
|---|---|---|---|
| Linux / C++20 | [Summary](scaling-ubuntu-latest-cxx20.md) · [raw](scaling-ubuntu-latest-cxx20.json) | [Summary](components-ubuntu-latest-cxx20.md) · [raw](components-ubuntu-latest-cxx20.json) | [Stages](profile-ubuntu-latest-cxx20-stages.md) · [raw](profile-ubuntu-latest-cxx20.json) |
| Linux / C++23 | [Summary](scaling-ubuntu-latest-cxx23.md) · [raw](scaling-ubuntu-latest-cxx23.json) | [Summary](components-ubuntu-latest-cxx23.md) · [raw](components-ubuntu-latest-cxx23.json) | [Stages](profile-ubuntu-latest-cxx23-stages.md) · [raw](profile-ubuntu-latest-cxx23.json) |
| macOS arm64 / C++20 | [Summary](scaling-macos-latest-cxx20.md) · [raw](scaling-macos-latest-cxx20.json) | [Summary](components-macos-latest-cxx20.md) · [raw](components-macos-latest-cxx20.json) | [Stages](profile-macos-latest-cxx20-stages.md) · [raw](profile-macos-latest-cxx20.json) |
| Windows x64 / C++20 | [Summary](scaling-windows-latest-cxx20.md) · [raw](scaling-windows-latest-cxx20.json) | [Summary](components-windows-latest-cxx20.md) · [raw](components-windows-latest-cxx20.json) | [Stages](profile-windows-latest-cxx20-stages.md) · [raw](profile-windows-latest-cxx20.json) |
| FreeBSD 14.3 VM / C++20 | [Summary](scaling-freebsd.md) · [raw](scaling-freebsd.json) | [Summary](components-freebsd.md) · [raw](components-freebsd.json) | [Stages](profile-freebsd-stages.md) · [raw](profile-freebsd.json) |

Profiling adds timing overhead. Its counters cover the server event-loop
lifetime, including startup, warmup and shutdown, rather than just the throughput
window. Inclusive stages overlap; exclusive times partition each reactor
lifetime, including blocking waits and descheduling. These are wall-clock
attributions, not CPU samples or per-request latency. Do not compare instrumented
throughput with uninstrumented Asio/libuv results. See
[profiling interpretation](../../profiling.md); each stage Markdown report also
has a neighboring JSON summary.

## Provenance and validation

- Source PR head: `72327e4a2f5b787deeb902d188e26c67f9799441`.
- Actual Actions checkout/merge recorded by every platform: `b1c994212888585ea06a71f7276463379314bd93`.
- [Manifest](manifest.json): 51 archived artifact files and their SHA-256 hashes, matched byte for byte against the downloaded Actions artifacts. Raw reports were not edited.
- **614 raw measurement records validated:** 75 fair backend trials, 180 scaling trials, 200 component trials, 15 instrumented trials, 128 paired component trials and 16 paired TCP trials. All required cells, trial identities, workload fields, checksums/counts, numeric consistency, ordering and published aggregates passed validation.
- Recomputed all nine paired summaries and bootstrap intervals, and all five derived profiling summaries. Every instrumented trial's exclusive-stage total equals its reactor lifetime. Combined TCP/profile records also match the duplicate raw JSONL evidence in the downloaded artifacts.
- All 85 recorded scaling source-file hashes match the checked source content: the 17 Windows files use CRLF checkout bytes; the other 68 use LF. Executable hashes are retained as emitted by the jobs. Binaries are not included in these artifacts, so their hashes cannot be independently recomputed from this archive.

| Platform | Reported CPU | Logical CPUs | Compiler |
|---|---|---:|---|
| Linux / C++20 | AMD EPYC 9V45 96-Core Processor | 4 | GCC 13.3.0 |
| Linux / C++23 | AMD EPYC 7763 64-Core Processor | 4 | GCC 13.3.0 |
| macOS arm64 / C++20 | Apple M1 (Virtual) | 3 | Clang 21.0.0 |
| Windows x64 / C++20 | AMD64 Family 25 Model 1 Stepping 1, AuthenticAMD | 4 | MSVC 1951 |
| FreeBSD 14.3 VM / C++20 | amd64 | 4 | Clang 19.1.7 |

The manifest lists 34 JSON report files: 29 containing raw records and five derived
profiling summaries; the remaining manifest-listed files are Markdown reports
and revision identifiers. Duplicate TCP/profile evidence directories remain in
the downloadable Actions artifacts. CPU model strings describe the host/virtual
hardware and do not establish dedicated physical cores. FreeBSD reports only
`amd64` as its CPU model. CPU affinity and frequency were not controlled.

These results establish successful measurement of the specified workloads at
the recorded revisions. They do not establish production readiness, portable
speedups, HTTP throughput, remote-network capacity or independent replication.
