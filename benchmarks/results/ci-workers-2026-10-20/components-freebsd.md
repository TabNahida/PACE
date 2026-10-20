| Component | Median ns/op | Min-max ns/op | Trials |
|---|---:|---:|---:|
| lazy_task | 17.97 | 17.82-19.77 | 5 |
| await_chain | 102.00 | 101.72-103.85 | 5 |
| empty_poll | 134.47 | 134.07-135.32 | 5 |
| yield | 49.25 | 48.44-49.70 | 5 |
| post | 59.70 | 59.28-60.48 | 5 |
| cross_thread_post | 70.62 | 69.22-89.98 | 5 |
| spawn_completion | 95.01 | 93.13-96.12 | 5 |
| zero_timer | 87.81 | 86.75-90.51 | 5 |

Each op has the component-specific definition saved in the JSON report.
These are repeated batch averages, not per-operation latency percentiles.
