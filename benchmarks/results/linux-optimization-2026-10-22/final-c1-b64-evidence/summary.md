| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 46,590 | 44,966–47,028 | 20.09 | 30.57 | 41.68 | 143.82 |
| asio | 46,354 | 45,442–47,573 | 20.33 | 28.53 | 39.09 | 328.86 |
| libuv | 46,990 | 46,111–47,702 | 20.25 | 28.12 | 40.67 | 114.70 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 4.629 | 4.594–4.637 | /proc/self/status.VmHWM_KiB |
| asio | 4.219 | 4.207–4.281 | /proc/self/status.VmHWM_KiB |
| libuv | 4.484 | 4.484–4.488 | /proc/self/status.VmHWM_KiB |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

3 trials per backend; one server thread; 1 persistent connections; 64-byte payload; one outstanding request per connection; 30000 measured + 1000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
