| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 340,618 | 338,097–346,554 | 77.59 | 156.74 | 181.13 | 7997.68 |
| asio | 326,745 | 318,660–328,338 | 94.77 | 110.62 | 145.31 | 7976.59 |
| libuv | 314,866 | 307,735–316,595 | 93.38 | 180.06 | 198.45 | 7908.47 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 21.266 | 21.199–21.297 | /proc/self/status.VmHWM_KiB |
| asio | 20.906 | 20.875–20.949 | /proc/self/status.VmHWM_KiB |
| libuv | 19.219 | 19.211–19.305 | /proc/self/status.VmHWM_KiB |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

7 trials per backend; one server thread; 32 persistent connections; 64-byte payload; one outstanding request per connection; 60000 measured + 5000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
