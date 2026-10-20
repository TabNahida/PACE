| Component | Median ns/op | Min-max ns/op | Trials |
|---|---:|---:|---:|
| lazy_task | 18.04 | 16.68-20.36 | 5 |
| await_chain | 100.47 | 100.18-102.24 | 5 |
| empty_poll | 258.91 | 257.47-261.17 | 5 |
| yield | 35.97 | 35.88-48.72 | 5 |
| post | 43.42 | 43.33-43.46 | 5 |
| cross_thread_post | 30.63 | 30.11-43.48 | 5 |
| spawn_completion | 74.31 | 73.61-74.85 | 5 |
| zero_timer | 68.76 | 68.09-69.70 | 5 |

Each op has the component-specific definition saved in the JSON report.
These are repeated batch averages, not per-operation latency percentiles.
