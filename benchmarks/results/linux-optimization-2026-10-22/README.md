# Local Linux runtime/TCP optimization — 22 October 2026

The finalized PACE implementation clearly improves **median (p50) TCP round-trip
latency** against both Asio and libuv for the selected 32-connection/64-byte local
workload. It also improves throughput over the original PACE implementation.
**There is a material tail-latency tradeoff:** PACE's p95/p99 are worse than Asio
and worse than the original PACE in this workload. This is a partial performance
win, not general superiority or satisfaction of the earlier release-wide gate.

## Measured bottlenecks and changes

1. The original Linux reactor rearmed `EPOLLONESHOT` with `epoll_ctl` after almost
   every completed receive. The reactor now uses persistent level-triggered
   interests, coalesces rearming on the owning thread, and reconciles genuinely
   idle interests before polling. Standalone `detail::poller` still defaults to
   its one-shot contract; other platforms retain their prior poller behavior.
2. TCP `read_some` previously speculated with another `recv` after every successful
   short read, normally receiving `EAGAIN` before it waited for readiness. A
   per-stream hint now waits for level readiness first after a short read. A full
   buffer retains the speculative fast path. The hint follows socket moves;
   empty-buffer operations, queued bytes and EOF retain their documented behavior.

The diagnostic workload is 32 clients × (10,000 measured + 1,000 warmup) requests,
64-byte payload, three trials. Counters cover the **entire reactor lifetime**,
including setup/warmup/shutdown; they are not counts per measured request.

| Diagnostic median | Original HEAD | Persistent only | Final |
|---|---:|---:|---:|
| `poller_update` calls | 351,977 | 33 | 33 |
| `recv` attempts | 704,004 | 703,993 | 352,032 |
| `io_arm` average inclusive ns/call | 270.9 | 39.7 | 39.7 |
| `poller_update` exclusive ms | 75.456 | 0.030 | 0.026 |
| `recv` exclusive ms | 181.271 | 180.725 | 138.770 |

This establishes the removed work: roughly 99.99% fewer poller updates and 50%
fewer receive attempts in this workload. These are intrusive wall-clock scopes,
not CPU samples. Kernel send/receive and scheduling still dominate; a blocked
poll is not evidence of CPU overhead. Instrumented throughput is **excluded** from
all Asio/libuv comparisons. Full diagnostics: [before](profile-before-stages.md),
[persistent only](profile-after-stages.md), [final](profile-final-stages.md), with
matching raw JSON reports beside them.

A separate [libc entry-point census](syscall-counts.json) provides a direct check
against Asio: with 32 clients × (1,000 measured + 1,000 warmup) requests, original
PACE made 63,990 `epoll_ctl(..., MOD, ...)` calls, final PACE 33, and Asio 1.
These totals include setup and teardown. The [LD_PRELOAD interposer](epoll_counts.c)
adds overhead, so none of that run's timing values support performance claims.
It intercepts libc calls, not all kernel operations: libuv's ADD/MOD registrations
were not observed, so its incomplete count is retained but excluded from total-
work comparisons. In particular, zero observed ADD/MOD does not mean zero work.

## Longer three-backend comparison

Every backend uses the same executable, one server thread, 32 persistent localhost
TCP connections, a 64-byte payload and one request in flight per client. Each
trial performs 60,000 measured + 5,000 warmup requests per client. All payload bytes
are checked and all 1,920,000 measured round trips are retained as latency samples.
Backend order rotates. Values below are medians of trial metrics; percentiles are
not pooled across trials. The raw JSON retains every trial and all min/max values.

### Session 1: 10 trials per backend

| Backend | Requests/s [min–max] | p50 µs [min–max] | p95 µs | p99 µs | Peak process MiB |
|---|---:|---:|---:|---:|---:|
| pace | 340,328 [338,424–342,380] | 77.713 [77.420–78.401] | 157.011 | 177.942 | 21.264 |
| asio | 327,696 [323,682–330,060] | 94.686 [94.216–94.924] | 109.531 | 141.217 | 20.926 |
| libuv | 313,891 [312,018–316,171] | 93.203 [92.724–93.432] | 179.798 | 197.630 | 19.213 |

Measured duration per process: 5.61–6.15 seconds. PACE/comparator p50 paired ratio vs asio: **0.8214** (exploratory bootstrap 95% interval 0.8191–0.8270). PACE/comparator p50 paired ratio vs libuv: **0.8343** (exploratory bootstrap 95% interval 0.8322–0.8399).

[Complete raw session 1](final-c32-b64-session1.json) · [metric spreads](final-c32-b64-session1-evidence/summary.json).

### Session 2: 7 trials per backend

| Backend | Requests/s [min–max] | p50 µs [min–max] | p95 µs | p99 µs | Peak process MiB |
|---|---:|---:|---:|---:|---:|
| pace | 340,618 [338,097–346,554] | 77.589 [76.875–77.961] | 156.739 | 181.127 | 21.266 |
| asio | 326,745 [318,660–328,338] | 94.768 [94.557–95.816] | 110.616 | 145.310 | 20.906 |
| libuv | 314,866 [307,735–316,595] | 93.379 [92.906–94.066] | 180.059 | 198.449 | 19.219 |

Measured duration per process: 5.54–6.24 seconds. PACE/comparator p50 paired ratio vs asio: **0.8162** (exploratory bootstrap 95% interval 0.8112–0.8199). PACE/comparator p50 paired ratio vs libuv: **0.8276** (exploratory bootstrap 95% interval 0.8240–0.8355).

[Complete raw session 2](final-c32-b64-session2.json) · [metric spreads](final-c32-b64-session2-evidence/summary.json).

The long 32-client scenario was selected **after** the five-scenario exploratory
matrix; it is not an independently predeclared workload search. The second session
uses the same frozen binary after an intervening build/test interval.
It is a repeat on the same host/day, not independent hardware validation.

The earlier suggested gate required at least 5% throughput improvement, a paired
95% lower bound above 1.05 and p99 no more than 5% worse than each comparator.
**The final build does not meet that gate against Asio.** The supported headline
is the substantial p50 reduction in this selected Linux workload, with the tail
latency cost shown in the same table.

## Alternating original/final PACE comparison

Ten pairs alternate before/after execution order. Both binaries use the same
compiler, flags, benchmark source, libraries and 32-client long workload. The
baseline is revision `e2e4cf2b2c3a79cae53d8cc6e12137293f04ca25`; its core sources
were preserved before building. All ten pairs favored final throughput.

| Metric | Original median | Final median | Paired final/original ratio | Exploratory 95% interval |
|---|---:|---:|---:|---:|
| requests_per_second | 309,356.366 | 341,289.672 | 1.1026 | 1.0907–1.1151 |
| p50_us | 100.463 | 77.527 | 0.7727 | 0.7651–0.7751 |
| p95_us | 115.862 | 156.776 | 1.3463 | 1.3240–1.3665 |
| p99_us | 147.817 | 179.842 | 1.1986 | 1.1737–1.2471 |

Comparing the variant medians, throughput improvement is accompanied by about
**35% worse p95 and 22% worse p99** than original PACE, despite its much lower p50.
These ratios of medians differ from the median of matched-pair ratios in the table.
[Raw alternating pairs](final-vs-head.json) preserve every latency record as well
as throughput. [comparisons.json](comparisons.json) adds matched-trial ratios for
all three-backend reports and all four original/final metrics. It is recomputable
with [summarize.py](summarize.py), which revalidates every record and uses 10,000
whole-pair bootstrap resamples, seed 0. Independence/representativeness assumptions
may fail under correlated host scheduling/frequency changes; the intervals are
exploratory, not portable guarantees.

## Other workloads: final build

Three rotated trials per backend, 1,000 warmup requests per connection. These
short checks show the workload boundaries and are not long-run superiority tests.
32-client/64-byte results are in the longer tables above.

| Clients / bytes / measured requests per client | Backend | Requests/s | p50 µs | p95 µs | p99 µs |
|---|---|---:|---:|---:|---:|
| [1 / 64 / 30,000](final-c1-b64.json) | pace | 46,590 | 20.089 | 30.566 | 41.680 |
| [1 / 64 / 30,000](final-c1-b64.json) | asio | 46,354 | 20.333 | 28.528 | 39.089 |
| [1 / 64 / 30,000](final-c1-b64.json) | libuv | 46,990 | 20.254 | 28.116 | 40.674 |
| [4 / 64 / 20,000](final-c4-b64.json) | pace | 154,915 | 24.372 | 33.023 | 42.427 |
| [4 / 64 / 20,000](final-c4-b64.json) | asio | 160,647 | 23.376 | 33.369 | 41.931 |
| [4 / 64 / 20,000](final-c4-b64.json) | libuv | 167,003 | 22.501 | 32.807 | 39.839 |
| [64 / 64 / 5,000](final-c64-b64.json) | pace | 339,618 | 171.110 | 336.341 | 371.653 |
| [64 / 64 / 5,000](final-c64-b64.json) | asio | 323,783 | 191.139 | 219.301 | 296.583 |
| [64 / 64 / 5,000](final-c64-b64.json) | libuv | 318,539 | 196.273 | 221.066 | 283.064 |
| [4 / 4096 / 10,000](final-c4-b4096.json) | pace | 138,516 | 27.268 | 37.848 | 58.906 |
| [4 / 4096 / 10,000](final-c4-b4096.json) | asio | 139,570 | 27.406 | 37.499 | 52.209 |
| [4 / 4096 / 10,000](final-c4-b4096.json) | libuv | 143,169 | 26.038 | 38.767 | 55.528 |

At 64 clients PACE again has a lower p50, with worse p95/p99 than both comparators.
At low concurrency and the 4 KiB payload there is no clear general advantage.
All five initial `explore-*.json` reports (persistent-only build), the
[short-read candidate](readiness-c32-b64.json), and its
[three-pair comparison](readiness-vs-persistent.json) are retained rather than
replacing unfavorable exploratory results. The candidate and final benchmark
binaries have the same SHA-256.

## Provenance, reproduction and limits

- Linux 6.18.40.1 Microsoft WSL2, Intel Core Ultra 7 270K Plus, 24 visible CPUs.
  GCC 13.3.0, C++20, `-O3 -DNDEBUG`, no LTO, instrumentation or sanitizers in the
  comparison executable. Asio 1.34.2 and static libuv 1.52.1.
- Final binary SHA-256:
  `e372cb913fde4ed53550f858b37c4c14ddcf97ac44f4054e73c89506442f11a2`.
  [build.json](build.json) records exact compilation commands, hashes, compiler,
  CPU/kernel, revision and the source-path mapping after the layering refactor.
- All timed processes ran serially. Other task agents paused compilation/testing
  during the measurement windows. CPU affinity, frequency and unrelated host/VM
  activity were not controlled. The results remain local closed-loop loopback
  measurements, including client/kernel/scheduler work; they do not measure
  HTTP/3, QUIC, DNS, multicore scaling, overload behavior or another platform.
- Peak process memory is Linux `VmHWM`, **including server, blocking clients,
  stacks and all stored latency samples**. It covers startup/warmup/shutdown and
  is read before merged-sample allocation. Linux accounting is approximate.
  PACE does not demonstrate lower memory here. The
  [current-image/launcher-contamination probe](memory-probe.json) passes.
  These numbers do not identify coroutine, allocator or isolated library costs.
- The source contains coroutine frames and node-based descriptor/token tables,
  but no allocator census was collected. Attributing the whole-process memory
  difference to those structures would exceed the evidence.

The retained [final patch](final-runtime.patch) plus the recorded baseline revision
reconstructs every measured core source exactly; [reproduce_build.py](reproduce_build.py)
checks the final source hashes before compiling. An independent rebuild with this
script reproduced both measured binaries byte-for-byte. It uses the original source paths
inside isolated snapshots, so it also works after the repository's layer moves.
Provide the dependency paths from your own xmake cache:

```sh
python3 benchmarks/results/linux-optimization-2026-10-22/reproduce_build.py \
  --asio-include /path/to/asio/1.34.2/include \
  --libuv-root /path/to/libuv/1.52.1 \
  --output build/linux-optimization-reproduce
python3 scripts/run_benchmarks.py \
  --executable build/linux-optimization-reproduce/final-bench --require-all \
  --repeats 10 --clients 32 --bytes 64 --requests 60000 --warmup 5000 \
  --output benchmark-results/linux-optimization-new-session.json
python3 scripts/compare_builds.py \
  --before build/linux-optimization-reproduce/before-bench \
  --after build/linux-optimization-reproduce/final-bench --tcp \
  --pairs 10 --clients 32 --bytes 64 --requests 60000 --warmup 5000 \
  --output benchmark-results/linux-optimization-new-pairs.json
python3 benchmarks/results/linux-optimization-2026-10-22/summarize.py
```

Re-run into fresh output paths; keep all trials, including regressions.
`SHA256SUMS` covers the retained evidence files.

## Regression validation and next priorities

The runtime suite passes in release and ASan/UBSan/LeakSanitizer builds, including
persistent poller interest replacement/removal and 600 readable descriptors that
exceed both 256-entry batches while some tasks suspend on timers. TCP core tests
pass with ASan/UBSan/LeakSanitizer, including the new short-read, queued-data,
move-construction/assignment, empty-buffer, full-buffer remainder and EOF case.
The benchmark runner's 37 Python tests pass. Full protocol/module validation is
recorded separately with the architecture change. No performance numbers from
sanitizer runs are used.

The next priority is per-connection and readiness-dispatch tracing to investigate
why p95/p99 regress. Scheduling fairness is a **hypothesis to test**, not an
established cause from these wall-clock scopes. Compare per-connection service
gaps and readiness/dispatch order before tuning batch sizes. Then collect server-
only allocation counts/bytes and lifetimes before attempting allocator changes
or making a memory claim. An independent open-loop workload and controlled CPU
placement would clarify capacity and tail behavior beyond this closed-loop test.

中文摘要：本地 Linux 的 32 连接、64 字节测试中，PACE 的 p50 明显低于 Asio 和
libuv，较优化前的自身吞吐提高约 10%。但自身 p95/p99 分别退化约 35%/22%，
且尾延迟仍输 Asio；这是一部分指标的明确提升，不能宣传为全面胜出。内存数据包含
客户端和所有延迟样本，未证明独立库内存优势。
