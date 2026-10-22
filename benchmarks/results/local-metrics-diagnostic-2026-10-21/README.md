# Initial memory measurement diagnostic — 21 October 2026

These four reports preserve the first 84 trials of the new multi-metric runner.
They are retained to explain a measurement correction, **not used to claim a
library memory advantage**.

Linux `getrusage(RUSAGE_SELF).ru_maxrss` retained a high-water mark from the Python
launcher across `exec`. Every backend in each workload consequently reported
the same 19.3–19.4 MiB floor. A smaller Python launcher produced an 11.3 MiB value
for a one-request run of the same binary. These values do not distinguish the
memory consumed by the benchmark executable from inherited pre-exec residency.

The throughput/latency trials completed and validated every response. They are
an exploratory snapshot; the final report reruns all metrics together after
correcting the memory source. Nothing is imputed or retroactively substituted.

| Report | Connections | Bytes | Measured requests per client | Trials per backend |
|---|---:|---:|---:|---:|
| [c1-b64](c1-b64.json) | 1 | 64 | 30,000 | 7 |
| [c4-b64](c4-b64.json) | 4 | 64 | 30,000 | 7 |
| [c32-b64](c32-b64.json) | 32 | 64 | 5,000 | 7 |
| [c4-b4096](c4-b4096.json) | 4 | 4,096 | 10,000 | 7 |

Each report includes all three backends (PACE, Asio 1.34.2, libuv 1.52.1), rotated
trial order, 1,000 warmup requests per client, the binary SHA-256, command lines
and machine metadata. Source: `c0252b666f2a515e2fd0bce81b31b065b1aa0060`;
the recorded dirty change only widens a WebSocket regression-test deadline and
is not linked into the benchmark. No concurrent compilation or protocol tests
ran during the measurement window.

中文：这批原始数据用于记录内存测量问题。Linux 的 `ru_maxrss` 继承了启动器的
历史峰值，不能据此比较三套库的内存占用。正式报告会在修正后重新运行完整对比，
保留本目录作为测量过程的证据。
