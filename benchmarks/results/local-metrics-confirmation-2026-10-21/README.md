# Longer TCP latency check — 21 October 2026

The [initial four-workload comparison](../local-metrics-2026-10-21/README.md)
showed lower p95 latency for PACE than libuv at 32 connections. We selected that
workload for a longer follow-up: **50,000 measured + 5,000 warmup requests per
client**, seven rotated trials per backend. This follow-up was selected after
seeing the short results; it is not an independently predeclared workload search.

Each of the 21 fresh processes measured 4.99–5.40 seconds. All 33,600,000 measured
round trips were verified and retained as latency samples. One server thread,
32 blocking clients, 64-byte payloads and pipeline depth one are identical for
PACE, Asio 1.34.2 and libuv 1.52.1.

## Complete results

Every cell is **median [min–max] across seven trials**. Percentiles are medians of
per-trial percentiles, not pooled percentiles. Ranges are observed spread, not
confidence intervals. Memory is whole-process peak MiB, including common clients,
server, stacks and sample storage, and is not isolated library allocation.

| Backend | Requests/s | p50 µs | p95 µs | p99 µs | Peak MiB |
|---|---:|---:|---:|---:|---:|
| PACE | 302,496 [296,540–304,564] | 100.968 [100.679–103.418] | 127.807 [122.467–130.194] | 197.432 [172.848–211.424] | 18.766 [18.715–18.816] |
| Asio | 315,445 [312,890–320,476] | 95.823 [95.332–97.055] | 122.448 [120.902–126.934] | 195.435 [171.265–201.311] | 18.441 [18.375–18.469] |
| libuv | 304,033 [299,273–306,512] | 94.743 [94.083–95.312] | 183.918 [183.316–186.438] | 218.189 [212.095–233.441] | 16.719 [16.711–17.035] |

PACE's median p95 is **30.5% lower than libuv's** (127.807 vs 183.918 µs);
all seven PACE values are below all seven libuv values in this session. PACE's
median p99 is 9.5% lower, but Asio's throughput and median p95/p99 remain better
than PACE's. PACE's peak process memory is higher than both comparators. The
README selects the p95 comparison and links to the complete performance report.

The longer workload stores ten times as many latency samples as the initial
32-client workload. Its higher peak memory is not a library regression; compare
the three backends within this workload. Throughput/latency exclude startup and
warmup; memory covers the current executable image through shutdown, before
allocating the merged sample vector. Linux VmHWM accounting is approximate.

## Provenance and limitations

The host, compiler and executable match the initial local comparison: WSL2 Linux,
Intel Core Ultra 7 270K Plus, GCC 13.3.0 C++20 release, no profiling or sanitizers.
No CPU affinity, fixed frequency or dedicated-host isolation was enforced. One
localhost session cannot establish cross-platform or production performance.
These closed-loop TCP trials do not measure HTTP/TLS processing or overload
latency and do not correct for coordinated omission.

Recorded revision: `3eb014ce014d11b780ef019b9c1936b77f8d81d6`. The working tree contained documentation,
charts and evidence changes; the original `git_status` is preserved in the JSON.
The measured benchmark source was unchanged. Executable SHA-256:
`bc163192cd134c53e3dd63e48a9d853ecb7cc4b866b2b1034917f4ff9d29f8b2`.

[Combined raw report](c32-b64.json) and [sidecar evidence](c32-b64-evidence/summary.md)
are copied byte-for-byte. [SHA256SUMS](SHA256SUMS) covers all five original files.
The combined report preserves exact commands, timestamps, every raw result and
all metric ranges, including maximum latency. The chart renderer validated every
record and recomputed all summaries before plotting.

```sh
python3 scripts/run_benchmarks.py --build-dir build/fair --require-all --repeats 7 \
  --clients 32 --bytes 64 --requests 50000 --warmup 5000 \
  --label local-linux-c32-b64-longer --output benchmark-results/longer/c32-b64.json
```

Use the [build commands](../../../docs/performance.md#reproduce-the-current-measurements)
to create an uninstrumented comparison binary first. Keep new measurements in a
new output directory and record the source and binary identity.

中文：这是看到初测结果后追加的较长复核，每轮约五秒。PACE 相对 libuv 的 p95
中位数低约 30.5%，但 Asio 的吞吐和尾延迟仍更好，PACE 的进程峰值内存仍更高。
完整记录全部保留；这一结果不能推广为所有平台或协议上的综合优势。
