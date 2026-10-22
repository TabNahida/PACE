| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 131,993 | 129,194–143,504 | 28.50 | 42.43 | 61.84 | 457.63 |
| asio | 130,509 | 126,354–132,390 | 29.24 | 44.43 | 63.06 | 448.26 |
| libuv | 135,953 | 127,675–163,856 | 26.96 | 40.19 | 61.01 | 436.52 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 4.977 | 4.906–5.020 | /proc/self/status.VmHWM_KiB |
| asio | 4.598 | 4.559–4.609 | /proc/self/status.VmHWM_KiB |
| libuv | 4.688 | 4.637–4.691 | /proc/self/status.VmHWM_KiB |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

7 trials per backend; one server thread; 4 persistent connections; 4096-byte payload; one outstanding request per connection; 10000 measured + 1000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
