| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 68,827 | 67,952–69,851 | 58.84 | 68.06 | 75.01 | 694.14 |
| asio | 70,212 | 69,719–70,934 | 56.90 | 66.87 | 72.83 | 551.58 |
| libuv | 67,935 | 64,497–69,158 | 42.10 | 85.92 | 99.64 | 506.39 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 4.906 | 4.848–4.969 | /proc/self/status.VmHWM_KiB |
| asio | 4.562 | 4.516–4.602 | /proc/self/status.VmHWM_KiB |
| libuv | 4.625 | 4.602–4.656 | /proc/self/status.VmHWM_KiB |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

5 trials per backend; one server thread; 4 persistent connections; 64-byte payload; one outstanding request per connection; 10000 measured + 1000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
