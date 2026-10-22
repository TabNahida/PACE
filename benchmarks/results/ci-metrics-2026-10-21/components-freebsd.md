| Component | Median ns/op | Min-max ns/op | Trials |
|---|---:|---:|---:|
| lazy_task | 18.49 | 18.16-21.03 | 5 |
| await_chain | 90.99 | 88.50-96.91 | 5 |
| empty_poll | 115.02 | 113.55-117.25 | 5 |
| yield | 37.38 | 36.04-38.20 | 5 |
| post | 49.14 | 48.13-50.96 | 5 |
| cross_thread_post | 65.32 | 63.21-68.04 | 5 |
| spawn_completion | 70.31 | 68.26-77.38 | 5 |
| zero_timer | 68.28 | 67.60-69.04 | 5 |

Each op has the component-specific definition saved in the JSON report.
These are repeated batch averages, not per-operation latency percentiles.
