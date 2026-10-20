| Component | Median ns/op | Min-max ns/op | Trials |
|---|---:|---:|---:|
| lazy_task | 7.80 | 7.74-8.06 | 5 |
| await_chain | 50.71 | 49.96-87.69 | 5 |
| empty_poll | 178.62 | 176.03-179.97 | 5 |
| yield | 26.02 | 25.77-26.08 | 5 |
| post | 30.72 | 30.51-31.15 | 5 |
| cross_thread_post | 25.44 | 25.14-35.98 | 5 |
| spawn_completion | 41.40 | 41.27-65.33 | 5 |
| zero_timer | 49.59 | 49.04-57.23 | 5 |

Each op has the component-specific definition saved in the JSON report.
These are repeated batch averages, not per-operation latency percentiles.
