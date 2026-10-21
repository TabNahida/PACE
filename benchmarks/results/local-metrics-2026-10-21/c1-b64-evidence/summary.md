| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 44,156 | 41,624–46,824 | 20.38 | 33.85 | 48.12 | 427.07 |
| asio | 44,488 | 42,940–45,980 | 20.41 | 33.08 | 47.99 | 194.13 |
| libuv | 45,832 | 44,543–46,516 | 20.30 | 31.25 | 47.36 | 345.64 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 4.676 | 4.598–4.695 | /proc/self/status.VmHWM_KiB |
| asio | 4.273 | 4.223–4.316 | /proc/self/status.VmHWM_KiB |
| libuv | 4.484 | 4.477–4.488 | /proc/self/status.VmHWM_KiB |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

7 trials per backend; one server thread; 1 persistent connections; 64-byte payload; one outstanding request per connection; 30000 measured + 1000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
