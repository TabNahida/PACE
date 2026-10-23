| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 339,618 | 336,750–344,969 | 171.11 | 336.34 | 371.65 | 882.15 |
| asio | 323,783 | 323,099–339,461 | 191.14 | 219.30 | 296.58 | 610.61 |
| libuv | 318,539 | 315,103–318,758 | 196.27 | 221.07 | 283.06 | 775.78 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 11.168 | 11.145–11.219 | /proc/self/status.VmHWM_KiB |
| asio | 10.867 | 10.863–10.883 | /proc/self/status.VmHWM_KiB |
| libuv | 8.418 | 8.188–8.598 | /proc/self/status.VmHWM_KiB |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

3 trials per backend; one server thread; 64 persistent connections; 64-byte payload; one outstanding request per connection; 5000 measured + 1000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
