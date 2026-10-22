| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 301,919 | 293,856–319,199 | 101.17 | 120.50 | 173.73 | 644.57 |
| asio | 321,471 | 317,361–324,284 | 95.52 | 117.31 | 162.65 | 581.59 |
| libuv | 302,702 | 299,576–308,278 | 93.76 | 182.35 | 209.62 | 2212.91 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 7.762 | 7.680–7.797 | /proc/self/status.VmHWM_KiB |
| asio | 7.438 | 7.391–7.453 | /proc/self/status.VmHWM_KiB |
| libuv | 5.973 | 5.750–6.160 | /proc/self/status.VmHWM_KiB |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

7 trials per backend; one server thread; 32 persistent connections; 64-byte payload; one outstanding request per connection; 5000 measured + 1000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
