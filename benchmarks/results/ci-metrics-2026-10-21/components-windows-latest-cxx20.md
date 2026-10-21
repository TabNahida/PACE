| Component | Median ns/op | Min-max ns/op | Trials |
|---|---:|---:|---:|
| lazy_task | 46.29 | 45.16-50.51 | 5 |
| await_chain | 228.53 | 227.41-236.50 | 5 |
| empty_poll | 1522.01 | 1505.79-1716.87 | 5 |
| yield | 35.82 | 34.68-36.71 | 5 |
| post | 44.04 | 43.60-44.49 | 5 |
| cross_thread_post | 151.27 | 138.44-212.51 | 5 |
| spawn_completion | 151.32 | 149.83-158.37 | 5 |
| zero_timer | 67.63 | 66.03-86.59 | 5 |

Each op has the component-specific definition saved in the JSON report.
These are repeated batch averages, not per-operation latency percentiles.
