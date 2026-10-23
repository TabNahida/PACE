| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 340,328 | 338,424–342,380 | 77.71 | 157.01 | 177.94 | 7472.91 |
| asio | 327,696 | 323,682–330,060 | 94.69 | 109.53 | 141.22 | 7420.17 |
| libuv | 313,891 | 312,018–316,171 | 93.20 | 179.80 | 197.63 | 7673.02 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 21.264 | 21.223–21.293 | /proc/self/status.VmHWM_KiB |
| asio | 20.926 | 20.875–20.961 | /proc/self/status.VmHWM_KiB |
| libuv | 19.213 | 19.156–19.219 | /proc/self/status.VmHWM_KiB |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

10 trials per backend; one server thread; 32 persistent connections; 64-byte payload; one outstanding request per connection; 60000 measured + 5000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
