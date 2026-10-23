| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 327,126 | 323,896–329,254 | 81.38 | 164.27 | 187.55 | 678.21 |
| asio | 320,684 | 320,560–322,430 | 95.58 | 118.37 | 158.92 | 6623.10 |
| libuv | 303,259 | 299,409–308,845 | 93.97 | 183.41 | 213.41 | 662.09 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 9.027 | 9.004–9.035 | /proc/self/status.VmHWM_KiB |
| asio | 8.691 | 8.688–8.707 | /proc/self/status.VmHWM_KiB |
| libuv | 6.930 | 6.922–6.934 | /proc/self/status.VmHWM_KiB |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

3 trials per backend; one server thread; 32 persistent connections; 64-byte payload; one outstanding request per connection; 10000 measured + 1000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
