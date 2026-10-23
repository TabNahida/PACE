| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p95 µs¹ | p99 µs¹ | Max µs¹ |
|---|---:|---:|---:|---:|---:|---:|
| pace | 154,915 | 151,871–156,676 | 24.37 | 33.02 | 42.43 | 410.03 |
| asio | 160,647 | 150,935–170,786 | 23.38 | 33.37 | 41.93 | 2915.60 |
| libuv | 167,003 | 153,566–178,170 | 22.50 | 32.81 | 39.84 | 386.94 |

¹ Median of each trial's percentile or maximum; not pooled percentiles or the overall worst request. JSON preserves every metric's min/max across trials.

| Backend | Whole-process peak memory MiB (median) | Min–max MiB | OS source |
|---|---:|---:|---|
| pace | 5.246 | 5.207–5.273 | /proc/self/status.VmHWM_KiB |
| asio | 4.852 | 4.832–4.914 | /proc/self/status.VmHWM_KiB |
| libuv | 4.965 | 4.938–4.969 | /proc/self/status.VmHWM_KiB |

Peak RSS/working set includes the server, identical blocking clients and stored latency samples. It includes startup/warmup and is read after shutdown, before result aggregation. Version 2 measures the current process image; unsupported providers report n/a, never zero. Linux VmHWM accounting is approximate. This does not isolate library footprint; compare only the same workload and OS.

3 trials per backend; one server thread; 4 persistent connections; 64-byte payload; one outstanding request per connection; 20000 measured + 1000 warmup requests per connection per trial.

Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.

Environment and commands: `environment.json`. Every run: `raw.jsonl`.
