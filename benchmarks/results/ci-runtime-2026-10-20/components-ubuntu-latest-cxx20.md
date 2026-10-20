| Component | Median ns/op | Min-max ns/op | Trials |
|---|---:|---:|---:|
| lazy_task | 14.92 | 14.17-24.58 | 5 |
| await_chain | 96.49 | 92.96-145.73 | 5 |
| empty_poll | 147.30 | 142.93-150.49 | 5 |
| yield | 36.58 | 35.36-51.97 | 5 |
| post | 44.03 | 43.02-44.56 | 5 |
| cross_thread_post | 61.19 | 55.38-91.48 | 5 |
| spawn_completion | 67.80 | 65.33-68.68 | 5 |
| zero_timer | 64.11 | 63.39-65.35 | 5 |

Each op has the component-specific definition saved in the JSON report.
These are repeated batch averages, not per-operation latency percentiles.
