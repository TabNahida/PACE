| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 302,496 | 296,540–304,564 | 100.97 | 127.81 | 197.43 | 7925.89 |
| asio | 315,445 | 312,890–320,476 | 95.82 | 122.45 | 195.44 | 5310.18 |
| libuv | 304,033 | 299,273–306,512 | 94.74 | 183.92 | 218.19 | 5138.08 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 18.766 | 18.715–18.816 | /proc/self/status.VmHWM_KiB |
| asio | 18.441 | 18.375–18.469 | /proc/self/status.VmHWM_KiB |
| libuv | 16.719 | 16.711–17.035 | /proc/self/status.VmHWM_KiB |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

7 trials per backend; one server thread; 32 persistent connections; 64-byte payload; one outstanding request per connection; 50000 measured + 5000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
