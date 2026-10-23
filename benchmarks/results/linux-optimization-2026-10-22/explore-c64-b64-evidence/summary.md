| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 324,505 | 321,610–326,548 | 177.74 | 348.91 | 393.15 | 5807.75 |
| asio | 323,656 | 314,164–324,301 | 192.44 | 227.44 | 287.21 | 846.85 |
| libuv | 311,243 | 307,804–317,200 | 197.96 | 241.45 | 316.68 | 762.83 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 11.176 | 11.168–11.223 | /proc/self/status.VmHWM_KiB |
| asio | 10.871 | 10.824–10.887 | /proc/self/status.VmHWM_KiB |
| libuv | 8.473 | 8.285–8.875 | /proc/self/status.VmHWM_KiB |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

3 trials per backend; one server thread; 64 persistent connections; 64-byte payload; one outstanding request per connection; 5000 measured + 1000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
