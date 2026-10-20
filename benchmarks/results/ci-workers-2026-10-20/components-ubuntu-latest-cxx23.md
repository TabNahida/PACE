| Component | Median ns/op | Min-max ns/op | Trials |
|---|---:|---:|---:|
| lazy_task | 16.78 | 16.62-17.48 | 5 |
| await_chain | 101.18 | 100.31-103.96 | 5 |
| empty_poll | 260.01 | 257.60-261.99 | 5 |
| yield | 36.07 | 35.87-36.61 | 5 |
| post | 43.31 | 43.17-43.45 | 5 |
| cross_thread_post | 31.90 | 31.33-32.87 | 5 |
| spawn_completion | 74.39 | 73.99-75.77 | 5 |
| zero_timer | 68.46 | 68.11-68.55 | 5 |

Each op has the component-specific definition saved in the JSON report.
These are repeated batch averages, not per-operation latency percentiles.
