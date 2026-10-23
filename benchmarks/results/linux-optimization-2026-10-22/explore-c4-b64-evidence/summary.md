| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 156,296 | 143,712–156,928 | 23.55 | 39.68 | 58.01 | 628.78 |
| asio | 151,950 | 149,347–152,428 | 24.34 | 41.32 | 56.53 | 602.73 |
| libuv | 152,361 | 152,145–166,691 | 24.38 | 36.28 | 54.49 | 233.24 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 5.254 | 5.230–5.258 | /proc/self/status.VmHWM_KiB |
| asio | 4.898 | 4.895–4.922 | /proc/self/status.VmHWM_KiB |
| libuv | 4.930 | 4.922–4.973 | /proc/self/status.VmHWM_KiB |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

3 trials per backend; one server thread; 4 persistent connections; 64-byte payload; one outstanding request per connection; 20000 measured + 1000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
