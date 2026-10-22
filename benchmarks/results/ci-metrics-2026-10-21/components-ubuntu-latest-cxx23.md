| Component | Median ns/op | Min-max ns/op | Trials |
|---|---:|---:|---:|
| lazy_task | 10.45 | 10.39-10.61 | 5 |
| await_chain | 70.10 | 68.62-84.21 | 5 |
| empty_poll | 103.24 | 102.97-126.01 | 5 |
| yield | 26.54 | 26.42-36.05 | 5 |
| post | 32.64 | 32.47-33.10 | 5 |
| cross_thread_post | 42.58 | 39.75-45.41 | 5 |
| spawn_completion | 49.14 | 47.92-75.78 | 5 |
| zero_timer | 46.92 | 46.68-47.41 | 5 |

Each op has the component-specific definition saved in the JSON report.
These are repeated batch averages, not per-operation latency percentiles.
