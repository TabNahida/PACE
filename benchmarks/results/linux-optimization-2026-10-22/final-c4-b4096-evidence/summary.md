| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 138,516 | 134,628–140,810 | 27.27 | 37.85 | 58.91 | 381.20 |
| asio | 139,570 | 137,433–141,395 | 27.41 | 37.50 | 52.21 | 373.74 |
| libuv | 143,169 | 140,119–157,564 | 26.04 | 38.77 | 55.53 | 402.77 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 4.969 | 4.949–4.969 | /proc/self/status.VmHWM_KiB |
| asio | 4.621 | 4.613–4.637 | /proc/self/status.VmHWM_KiB |
| libuv | 4.641 | 4.633–4.688 | /proc/self/status.VmHWM_KiB |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

3 trials per backend; one server thread; 4 persistent connections; 4096-byte payload; one outstanding request per connection; 10000 measured + 1000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
