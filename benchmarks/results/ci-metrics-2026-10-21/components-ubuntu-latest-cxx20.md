| Component | Median ns/op | Min-max ns/op | Trials |
|---|---:|---:|---:|
| lazy_task | 17.34 | 17.22-28.10 | 5 |
| await_chain | 96.65 | 95.53-140.04 | 5 |
| empty_poll | 483.83 | 481.62-486.20 | 5 |
| yield | 38.03 | 37.76-38.36 | 5 |
| post | 47.03 | 46.95-47.71 | 5 |
| cross_thread_post | 34.39 | 33.99-58.38 | 5 |
| spawn_completion | 76.62 | 76.01-119.28 | 5 |
| zero_timer | 72.88 | 72.17-87.75 | 5 |

Each op has the component-specific definition saved in the JSON report.
These are repeated batch averages, not per-operation latency percentiles.
