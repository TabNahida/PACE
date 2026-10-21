| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 36,169 | 35,414–37,360 | 104.20 | 174.10 | 244.30 | 753.90 |
| asio | 37,280 | 36,513–37,754 | 104.90 | 133.60 | 171.20 | 437.80 |
| libuv | 37,960 | 37,650–39,163 | 103.20 | 161.60 | 242.30 | 707.60 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 6.750 | 6.742–6.758 | GetProcessMemoryInfo.PeakWorkingSetSize |
| asio | 6.738 | 6.734–6.738 | GetProcessMemoryInfo.PeakWorkingSetSize |
| libuv | 7.785 | 7.777–7.805 | GetProcessMemoryInfo.PeakWorkingSetSize |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

5 trials per backend; one server thread; 4 persistent connections; 64-byte payload; one outstanding request per connection; 10000 measured + 1000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
