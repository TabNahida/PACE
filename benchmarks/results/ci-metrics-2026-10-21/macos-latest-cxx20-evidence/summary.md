| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 47,384 | 41,070–57,630 | 80.38 | 138.96 | 253.96 | 3962.75 |
| asio | 40,271 | 37,350–48,839 | 81.08 | 168.12 | 424.12 | 5030.33 |
| libuv | 57,233 | 47,353–82,944 | 63.79 | 113.12 | 175.50 | 3699.04 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 6.312 | 6.312–6.328 | task_info(MACH_TASK_BASIC_INFO).resident_size_max_bytes |
| asio | 6.297 | 6.281–6.297 | task_info(MACH_TASK_BASIC_INFO).resident_size_max_bytes |
| libuv | 6.281 | 6.266–6.281 | task_info(MACH_TASK_BASIC_INFO).resident_size_max_bytes |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

5 trials per backend; one server thread; 4 persistent connections; 64-byte payload; one outstanding request per connection; 10000 measured + 1000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
