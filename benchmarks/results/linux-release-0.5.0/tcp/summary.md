| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 329,097 | 319,851–333,075 | 79.78 | 162.68 | 200.89 | 10120.78 |
| asio | 315,704 | 308,084–319,800 | 96.79 | 123.02 | 167.79 | 10137.84 |
| libuv | 302,783 | 296,177–307,197 | 94.06 | 182.69 | 215.17 | 10279.88 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 21.367 | 21.340–21.414 | /proc/self/status.VmHWM_KiB |
| asio | 20.943 | 20.914–20.988 | /proc/self/status.VmHWM_KiB |
| libuv | 19.250 | 19.176–19.492 | /proc/self/status.VmHWM_KiB |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

10 trials per backend; one server thread; 32 persistent connections; 64-byte payload; one outstanding request per connection; 60000 measured + 5000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
