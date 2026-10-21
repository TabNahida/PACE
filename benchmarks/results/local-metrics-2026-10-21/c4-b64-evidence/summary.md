| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 148,906 | 147,223–156,799 | 24.85 | 36.71 | 53.48 | 587.97 |
| asio | 151,258 | 141,820–163,644 | 24.65 | 36.21 | 52.91 | 395.69 |
| libuv | 157,434 | 149,943–183,638 | 23.72 | 35.19 | 51.60 | 599.53 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 5.543 | 5.473–5.598 | /proc/self/status.VmHWM_KiB |
| asio | 5.176 | 5.129–5.223 | /proc/self/status.VmHWM_KiB |
| libuv | 5.266 | 5.211–5.270 | /proc/self/status.VmHWM_KiB |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

7 trials per backend; one server thread; 4 persistent connections; 64-byte payload; one outstanding request per connection; 30000 measured + 1000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
