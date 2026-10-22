| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 112,249 | 101,295–114,728 | 35.21 | 40.62 | 46.40 | 568.58 |
| asio | 116,049 | 104,279–120,067 | 33.91 | 39.12 | 44.67 | 562.48 |
| libuv | 109,598 | 109,300–115,003 | 29.92 | 49.42 | 55.07 | 975.27 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 5.039 | 4.961–5.043 | /proc/self/status.VmHWM_KiB |
| asio | 4.711 | 4.699–4.730 | /proc/self/status.VmHWM_KiB |
| libuv | 4.777 | 4.746–4.785 | /proc/self/status.VmHWM_KiB |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

5 trials per backend; one server thread; 4 persistent connections; 64-byte payload; one outstanding request per connection; 10000 measured + 1000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
