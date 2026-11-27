| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 227,410 | 224,496–229,171 | 31.71 | 60.38 | 86.75 | 136.17 |
| asio | 190,933 | 188,009–192,276 | 39.29 | 68.00 | 90.79 | 155.38 |
| libuv | 248,687 | 246,947–250,917 | 29.12 | 55.79 | 80.50 | 127.62 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 6.531 | 6.516–6.547 | task_info(MACH_TASK_BASIC_INFO).resident_size_max_bytes |
| asio | 6.453 | 6.453–6.469 | task_info(MACH_TASK_BASIC_INFO).resident_size_max_bytes |
| libuv | 6.422 | 6.422–6.438 | task_info(MACH_TASK_BASIC_INFO).resident_size_max_bytes |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

5 trials per backend; one server thread; 8 persistent connections; 64-byte payload; one outstanding request per connection; 5000 measured + 500 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
