| Component | Median ns/op | Min-max ns/op | Trials |
|---|---:|---:|---:|
| lazy_task | 13.96 | 13.81-14.92 | 5 |
| await_chain | 64.05 | 62.39-64.54 | 5 |
| empty_poll | 12094.58 | 12034.22-12121.30 | 5 |
| yield | 5.03 | 4.85-5.58 | 5 |
| post | 7.38 | 6.77-7.67 | 5 |
| cross_thread_post | 7.27 | 6.54-8.20 | 5 |
| spawn_completion | 18.32 | 17.54-19.15 | 5 |
| zero_timer | 28.25 | 27.77-29.52 | 5 |

Each op has the component-specific definition saved in the JSON report.
These are repeated batch averages, not per-operation latency percentiles.
