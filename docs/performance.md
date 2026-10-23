# Local Linux performance · 22 October 2026

[Documentation](README.md) · [Benchmark methodology](../benchmarks/README.md) · [Architecture](architecture.md)

PACE now demonstrates a repeatable **median round-trip latency advantage over
both Asio and libuv in the measured 32-connection workload**. It does not lead
every metric: Asio has substantially lower p95/p99, and PACE still has the
largest whole-process peak memory. Every new measurement on this page is from
local Linux; earlier cross-platform studies are [historical evidence](performance-history.md).

## Complete long-run comparison

One server thread, 32 loopback TCP connections, 64-byte messages, one outstanding
request per connection, **60,000 measured + 5,000 warmup requests per client**.
Ten rotating trials per backend, each about six seconds. All response bytes and
completed-request counts are checked. Values are medians across trials; latency
percentiles are medians of each trial's percentile, not pooled samples.

First measurement window, ten trials per backend:

| Backend | Requests/s | p50 µs | p95 µs | p99 µs | Peak process MiB |
|---|---:|---:|---:|---:|---:|
| PACE | 340,328 | 77.71 | 157.01 | 177.94 | 21.264 |
| Asio | 327,696 | 94.69 | 109.53 | 141.22 | 20.926 |
| libuv | 313,891 | 93.20 | 179.80 | 197.63 | 19.213 |

PACE's p50 is **17.9% lower than Asio and 16.6% lower than libuv**; all ten PACE
p50 values are below every comparator p50 in this session. Throughput medians
are 3.9% and 8.4% higher, respectively. These observations describe this host and
workload, not a general ranking of the libraries.

A second window repeats the same frozen binary and workload after an intervening
build/test interval, with seven rotating trials per backend:

| Backend | Requests/s | p50 µs | p95 µs | p99 µs | Peak process MiB |
|---|---:|---:|---:|---:|---:|
| PACE | 340,618 | 77.59 | 156.74 | 181.13 | 21.266 |
| Asio | 326,745 | 94.77 | 110.62 | 145.31 | 20.906 |
| libuv | 314,866 | 93.38 | 180.06 | 198.45 | 19.219 |

PACE's p50 is again lower, by **18.1% versus Asio and 16.9% versus libuv**.
The throughput gain, worse tails than Asio and higher memory also recur.
This is repetition on the same host and day, not independent hardware validation.
[Raw second-window JSON](../benchmarks/results/linux-optimization-2026-10-22/final-c32-b64-session2.json).

In the first window, PACE's p95 is **43.3% higher than Asio**, and its p99 is
**26.0% higher**. Both are lower than libuv's, but PACE cannot claim a universal
tail-latency advantage. It also does not meet the older goal of at least 5%
throughput improvement over Asio with p99 within 5%.

The ten alternating **old-PACE/new-PACE** pairs show the cost of the change
itself: throughput rises from 309,356 to 341,290 requests/s (paired median gain
10.26%, exploratory bootstrap 95% interval 9.07–11.51%, all ten pairs improve),
and p50 falls from 100.46 to 77.53 µs. However, p95 rises from 115.86 to 156.78 µs
(35.3%) and p99 from 147.82 to 179.84 µs (21.7%). The default hot path therefore
trades tail latency for throughput and median latency on this workload.
[Raw before/after pairs](../benchmarks/results/linux-optimization-2026-10-22/final-vs-head.json).

[Raw long-run JSON](../benchmarks/results/linux-optimization-2026-10-22/final-c32-b64-session1.json),
[all workload results, ranges, before/after pairs and reproducible builds](../benchmarks/results/linux-optimization-2026-10-22/README.md).
The 32-client case was selected after exploratory runs; it is not a predeclared
independent workload search. Unfavorable outcomes are retained in the archive.

## What was slowing PACE down

Two changes reduce work in the TCP hot path without changing the public API:

1. **Persistent Linux readiness interests.** The old reactor used EPOLLONESHOT
   and rearmed each receive through `epoll_ctl`. The reactor now retains a
   level-triggered subscription when a resumed coroutine waits on the same
   direction, and reconciles changed/idle directions before polling. In the
   32-client diagnostic, `poller_update` calls fell from about **351,977 to 33**.
   Generation tokens still reject stale events; idle writable interests are
   removed to avoid spinning. The standalone poller retains its one-shot default;
   kqueue and Windows keep their existing backends.
2. **Readiness after short TCP reads.** A short read commonly drained the receive
   queue, yet the next operation immediately called `recv` again, usually getting
   EAGAIN. A private per-stream hint now waits for level readiness first. Already
   queued bytes and EOF are still detected by the poller and verified by `recv`;
   full-buffer reads retain the immediate path. Empty buffers and moves have
   explicit regression coverage. Diagnostic receive calls fall from 704,004 to
   352,032 for the same 352,000 warmup-plus-measured round trips.

The initial instrumented baseline spent about 6.1% of reactor wall time in poller
updates, 14.6% in receive calls and 67.7% in sends. These are **instrumented
wall-clock diagnostics**, including kernel wait/descheduling and startup/warmup;
they are not CPU samples and must not be compared directly with uninstrumented
Asio/libuv throughput. Kernel TCP/send work remains the largest observed cost.
The archive includes separate diagnostics for the baseline, persistent-interest
intermediate version, and final short-read version.

The first optimization is Linux-specific. The short-read hint uses level-readiness
semantics but its performance has only been measured on Linux. Protocol layering
and DNS/QUIC/HTTP3 additions are separate functional changes; these TCP benchmarks
do not establish their speed.

## Memory and measurement limits

Peak memory is Linux `/proc/self/status` VmHWM for the complete executable image.
It includes the server, blocking client threads, application connection buffers,
warmup and stored latency samples. It is **not isolated library memory**. PACE
still records more memory than both comparators; no allocation census was taken,
so the difference cannot be attributed quantitatively to coroutine frames or the
reactor's descriptor maps. No memory advantage is claimed.

This is local WSL2 Linux on the same machine, GCC 13.3.0, C++20, `-O3`, with
Asio 1.34.2 and libuv 1.52.1. Profiling and sanitizers are disabled in comparative
runs. All timing runs are serial; concurrent compilation and protocol testing
were paused. No fixed frequency, CPU affinity or dedicated host was enforced.
Closed-loop localhost tests do not measure overload, production networks,
connection setup, multiworker scaling, UDP, TLS, HTTP, QUIC or HTTP/3 performance.
Observed ranges and exploratory bootstrap intervals do not remove these limits.

## Reproduce the current measurements

```sh
xmake f -c -m release -o build/fair --benchmarks=y --profiling=n --sanitizers=n -y
xmake -y pace_bench
python3 scripts/run_benchmarks.py --build-dir build/fair --require-all \
  --repeats 10 --clients 32 --bytes 64 --requests 60000 --warmup 5000 \
  --label local-linux-confirmation --output benchmark-results/local-linux-confirmation.json
```

Keep new measurements in a new output directory. The evidence archive records
source/binary hashes, compiler commands, exact workloads, raw trials and the
before/after baseline. Its `reproduce_build.py` reconstructs the recorded baseline
and final benchmark, including the source-directory migration. Use
`python3 scripts/run_profile.py --input REPORT.json` only for separately built
`--profiling=y` diagnostic reports. See [profiling](../benchmarks/profiling.md)
and [benchmark timing/memory semantics](../benchmarks/README.md).

中文结论：本次所有新性能证据都来自本地 Linux。32 连接、64 字节负载下，PACE
的 p50 相对 Asio/libuv 分别降低约 17.9%/16.6%，吞吐中位数也更高；但 Asio
的 p95/p99 更低，PACE 的整进程峰值内存仍更高。优化减少了 reactor 重复注册和
无效 recv。第二段运行窗口的 7 轮复核得到相同方向的结果，p50 分别降低约
18.1%/16.9%；不能推广成所有协议、负载或平台全面领先。
