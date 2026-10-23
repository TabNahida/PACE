| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 337,916 | 331,294–339,939 | 77.53 | 157.21 | 183.84 | 870.37 |
| asio | 314,663 | 310,606–323,731 | 95.90 | 127.50 | 180.24 | 5605.66 |
| libuv | 304,779 | 295,741–307,280 | 93.22 | 181.69 | 210.72 | 1242.23 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 9.020 | 8.988–9.043 | /proc/self/status.VmHWM_KiB |
| asio | 8.699 | 8.695–8.723 | /proc/self/status.VmHWM_KiB |
| libuv | 6.934 | 6.930–6.961 | /proc/self/status.VmHWM_KiB |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

3 trials per backend; one server thread; 32 persistent connections; 64-byte payload; one outstanding request per connection; 10000 measured + 1000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
