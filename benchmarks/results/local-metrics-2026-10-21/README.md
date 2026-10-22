# Local TCP comparison with current-image memory — 21 October 2026

These are **84 fresh-process TCP echo trials**: four preselected workloads,
three backends and seven rotated trials per backend per workload. PACE's clearest
observed advantage is lower p95 latency than libuv with 32 connections and 64-byte
messages. It does not lead the complete comparison: Asio has lower tail latency
in that workload, and PACE has the highest median process memory in all four.

[Measurement methodology](../../README.md) · [Complete performance discussion](../../../docs/performance.md)

The README now uses the [longer follow-up](../local-metrics-confirmation-2026-10-21/README.md),
which repeats the selected 32-client workload with 50,000 requests per client.
The original short trials below remain unchanged.

## Environment and provenance

- Linux 6.18.40.1 Microsoft WSL2, x86_64, glibc 2.39; Intel Core Ultra 7 270K Plus;
  24 reported logical CPUs. This is one local host and one measurement session.
- GCC 13.3.0, C++20, release (`NDEBUG`), no PACE profiling; standalone Asio 1.34.2
  and libuv 1.52.1. All backends reside in the same benchmark executable.
- Recorded source revision: `eebf34ec798d0a156e0326098dac47a86af05107`.
  Its change after `86c33a2` affects only the Python validation fixture, so the
  measured C++ source and binary are unchanged from that build.
- Executable SHA-256:
  `bc163192cd134c53e3dd63e48a9d853ecb7cc4b866b2b1034917f4ff9d29f8b2`.
  This hash was independently recomputed from the local executable after all
  trials and matches every report. The executable itself is not archived here.
- `c1-b64` and `c4-b64` record an empty `git_status`. `c32-b64` and `c4-b4096`
  record `M tests/test_benchmark_runner.py`: the separate CI-test mock fix was
  being prepared. No benchmark source or measured binary changed. The original
  per-workload status is retained verbatim; the whole session is not described
  as having a clean working tree.
- Each JSON records its start/end UTC time, full commands, workload, compiler,
  dependency versions, source revision and executable hash. No CPU affinity,
  frequency, or dedicated-host isolation was enforced.

## Workloads and measurement boundaries

Every trial uses one server thread, one blocking client thread per persistent
connection, one outstanding request per connection, `TCP_NODELAY` at both ends,
and byte-for-byte response verification. Backend order rotates each cycle.
The 24 reported logical CPUs do not mean 24 PACE workers: all three libraries
use **one server thread** in this comparison.

| Workload | Connections | Payload | Measured requests per connection | Warmup per connection | Trials per backend |
|---|---:|---:|---:|---:|---:|
| [c1-b64](c1-b64.json) | 1 | 64 B | 30,000 | 1,000 | 7 |
| [c4-b64](c4-b64.json) | 4 | 64 B | 30,000 | 1,000 | 7 |
| [c32-b64](c32-b64.json) | 32 | 64 B | 5,000 | 1,000 | 7 |
| [c4-b4096](c4-b4096.json) | 4 | 4,096 B | 10,000 | 1,000 | 7 |

All 7,350,000 measured requests have one retained latency sample. Throughput and
latency exclude startup, connection establishment and warmup. Throughput measures
verified round trips divided by elapsed time from the shared start gate until all
clients join. Per-request latency includes send, receive and payload checking.
These are closed-loop observations, with no coordinated-omission correction.

Memory uses version 2 `/proc/self/status.VmHWM_KiB`, converted to bytes. It belongs
to the **current executable image**, avoiding POSIX `getrusage` inheritance of a
Python launcher's pre-exec peak. The reading is taken after client/server shutdown
and before allocating the duplicate merged sample array. Its lifetime window
includes startup and warmup, server, identical blocking clients, thread stacks,
runtime state and stored latency samples. It is **not isolated library footprint,
steady-state memory, PSS, allocation count or per-connection cost**. Linux RSS
accounting is approximate. Compare memory within one workload; different request
counts allocate different amounts of sample storage.

Earlier version 1 trials remain in the [separate diagnostic archive](../local-metrics-diagnostic-2026-10-21/README.md).
They were not rewritten or used in this table; their inherited POSIX peak-memory
figures cannot support benchmark-image or library memory claims.

## All backends and metrics

Each cell is **median [minimum–maximum] across seven trials**. A median of trial
p95/p99 values is not a pooled percentile. Ranges are observed spread, not
confidence intervals. Throughput is requests/s; latencies are microseconds;
memory is MiB (1,048,576 bytes). Raw JSON also retains each trial's maximum
latency and the summary's median and overall worst maximum.

| Workload | Backend | Requests/s | p50 µs | p95 µs | p99 µs | Peak process MiB |
|---|---|---:|---:|---:|---:|---:|
| [1 clients · 64 B](c1-b64.json) | PACE | 44,156 [41,624–46,824] | 20.375 [19.895–20.899] | 33.848 [29.860–37.707] | 48.124 [46.244–56.873] | 4.676 [4.598–4.695] |
| [1 clients · 64 B](c1-b64.json) | Asio | 44,488 [42,940–45,980] | 20.408 [20.269–20.716] | 33.083 [30.348–34.841] | 47.988 [46.310–48.761] | 4.273 [4.223–4.316] |
| [1 clients · 64 B](c1-b64.json) | libuv | 45,832 [44,543–46,516] | 20.298 [20.127–20.408] | 31.248 [29.458–33.254] | 47.362 [46.127–48.569] | 4.484 [4.477–4.488] |
| [4 clients · 64 B](c4-b64.json) | PACE | 148,906 [147,223–156,799] | 24.853 [23.709–25.896] | 36.712 [35.899–38.426] | 53.480 [52.209–55.751] | 5.543 [5.473–5.598] |
| [4 clients · 64 B](c4-b64.json) | Asio | 151,258 [141,820–163,644] | 24.654 [23.287–25.811] | 36.211 [34.591–39.480] | 52.913 [51.202–59.282] | 5.176 [5.129–5.223] |
| [4 clients · 64 B](c4-b64.json) | libuv | 157,434 [149,943–183,638] | 23.719 [16.033–25.247] | 35.194 [34.406–36.638] | 51.599 [48.787–54.346] | 5.266 [5.211–5.270] |
| [32 clients · 64 B](c32-b64.json) | PACE | 301,919 [293,856–319,199] | 101.173 [96.791–102.021] | 120.505 [115.528–141.270] | 173.726 [156.032–220.540] | 7.762 [7.680–7.797] |
| [32 clients · 64 B](c32-b64.json) | Asio | 321,471 [317,361–324,284] | 95.519 [95.192–95.820] | 117.309 [114.704–120.699] | 162.652 [147.898–184.397] | 7.438 [7.391–7.453] |
| [32 clients · 64 B](c32-b64.json) | libuv | 302,702 [299,576–308,278] | 93.756 [92.836–94.573] | 182.355 [180.302–183.561] | 209.623 [205.303–222.771] | 5.973 [5.750–6.160] |
| [4 clients · 4096 B](c4-b4096.json) | PACE | 131,993 [129,194–143,504] | 28.498 [25.231–29.256] | 42.434 [39.170–44.427] | 61.837 [57.790–81.748] | 4.977 [4.906–5.020] |
| [4 clients · 4096 B](c4-b4096.json) | Asio | 130,509 [126,354–132,390] | 29.239 [28.042–30.217] | 44.426 [41.346–45.464] | 63.057 [59.224–67.945] | 4.598 [4.559–4.609] |
| [4 clients · 4096 B](c4-b4096.json) | libuv | 135,953 [127,675–163,856] | 26.956 [19.305–30.327] | 40.194 [38.463–46.311] | 61.013 [58.753–66.451] | 4.688 [4.637–4.691] |

## Advantages, costs and uncertainty

At **32 connections × 64 B**, PACE's median p95 is **120.505 µs versus libuv's
182.355 µs**, a **33.9% reduction**. The PACE trial range is 115.528–141.270 µs,
compared with 180.302–183.561 µs for libuv. All seven PACE p95 values are below
all seven libuv values in this session. This is the most consistent positive
result in this matrix and motivated the separately archived longer check. The
current README panel uses that follow-up, not these shorter-trial numbers.

The same workload's median p99 is **173.726 versus 209.623 µs**, a **17.1%
reduction** against libuv. PACE is lower in six of seven same-cycle comparisons,
but the ranges overlap (156.032–220.540 versus 205.303–222.771 µs); one PACE trial
is slightly worse than the corresponding libuv trial. These rotated sequential
runs are not simultaneous matched pairs. The p99 result is less consistent than
p95 and should not be described as an established general advantage.

The cost is visible alongside the latency result. Against libuv in this workload,
PACE's throughput median is **0.26% lower**, and peak process memory is **30.0%
higher**. Asio has the highest throughput and lower p95/p99 than PACE. PACE's
p95 is 2.7% higher and p99 6.8% higher than Asio. The selected libuv comparison
therefore does not make PACE fastest or lowest-latency among all three libraries.

PACE has the highest median process memory in **all four** workloads, about
4.4–9.4% above Asio and 4.3–30.0% above libuv. No memory advantage is supported.
At one and four connections with 64-byte payloads, PACE's median throughput is
below both comparators; p99 is also slightly higher. At four connections with
4,096-byte payloads, the 1.1% throughput lead over Asio has overlapping ranges;
libuv remains ahead in the median. That small lead is unsuitable as the main
README performance claim.

Every measured trial is short: approximately **0.244–0.846 seconds** across the
matrix. Seven repeats on one WSL2 host do not establish stability across sessions,
operating systems, remote networks, real HTTP traffic or server capacity under
open-loop load. The proposed release criteria require longer repeated sessions
on controlled hardware. Describe the selected p95 result as an observed local
workload advantage, not proof that PACE broadly outperforms Asio and libuv.

## Reproduce and audit

Configure an uninstrumented release build with the comparison dependencies, then
check the memory provider independently. Run each workload into a new destination:

```sh
xmake f -m release -o build/fair --cxxstandard=cxx20 --benchmarks=y --profiling=n --sanitizers=n --tls=n --http2=n -y
xmake -y pace_bench
python3 scripts/run_benchmarks.py --build-dir build/fair --check-memory
python3 scripts/run_benchmarks.py --build-dir build/fair --require-all --repeats 7 \
  --clients 32 --bytes 64 --requests 5000 --warmup 1000 \
  --label local-linux-c32-b64 --output benchmark-results/reproduction/c32-b64.json
```

Use the other three workloads' request/client/payload values from the table and
retain every outcome. Current source may differ from the archived revision;
record its identity rather than treating new numbers as the same experiment.

The four combined JSON files and their `*-evidence/` directories were copied
**byte-for-byte** from completed local runs. [SHA256SUMS](SHA256SUMS) covers all
20 original files. Each evidence directory contains `raw.jsonl`, `environment.json`,
`summary.json` and the runner's full Markdown table. Validation checked all 84
records, all four summaries recomputed exactly from raw data, every JSONL copy,
every environment/summary sidecar, backend rotation and the executable hash.

## 中文阅读提示

这份报告公开全部四组负载、三个后端与七轮范围。32 连接、64 字节消息时，
PACE 相对 libuv 的 p95 中位数低 33.9%，七轮范围没有重叠；p99 中位数低 17.1%，
但范围重叠且有一轮略差。该场景中 Asio 的尾延迟仍更低；PACE 对 libuv 的吞吐
基本持平，峰值进程内存高约 30%。PACE 在四组负载中的内存中位数都最高。

内存包括服务端、客户端、线程栈和延迟样本，不是库本身的独立占用。
所有单轮测量都不足一秒，结论仅适用于本地这次 TCP echo 实验，不能推广到
所有平台、HTTP 服务或实际生产负载。表中方括号是实测最小值到最大值，
不是统计置信区间。
