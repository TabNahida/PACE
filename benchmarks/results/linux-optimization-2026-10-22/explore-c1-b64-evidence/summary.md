| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 45,611 | 43,567–46,224 | 20.38 | 31.67 | 39.17 | 184.15 |
| asio | 44,901 | 43,362–45,287 | 20.64 | 31.75 | 41.05 | 182.22 |
| libuv | 47,501 | 45,838–48,907 | 19.92 | 27.37 | 38.91 | 147.31 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 4.641 | 4.602–4.656 | /proc/self/status.VmHWM_KiB |
| asio | 4.242 | 4.242–4.320 | /proc/self/status.VmHWM_KiB |
| libuv | 4.477 | 4.449–4.488 | /proc/self/status.VmHWM_KiB |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

3 trials per backend; one server thread; 1 persistent connections; 64-byte payload; one outstanding request per connection; 30000 measured + 1000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
