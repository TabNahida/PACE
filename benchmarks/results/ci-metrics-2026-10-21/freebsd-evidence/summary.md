| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 359,152 | 353,157–371,582 | 9.96 | 12.09 | 33.94 | 468.88 |
| asio | 347,325 | 337,522–350,514 | 10.52 | 13.21 | 35.13 | 171.26 |
| libuv | 410,070 | 408,244–420,480 | 8.94 | 10.63 | 31.39 | 143.62 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | n/a | n/a | no verified current-image peak resident memory provider on this platform |
| asio | n/a | n/a | no verified current-image peak resident memory provider on this platform |
| libuv | n/a | n/a | no verified current-image peak resident memory provider on this platform |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

5 trials per backend; one server thread; 4 persistent connections; 64-byte payload; one outstanding request per connection; 10000 measured + 1000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
