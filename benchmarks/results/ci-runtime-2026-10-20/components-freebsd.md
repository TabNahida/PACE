| Component | Median ns/op | Min-max ns/op | Trials |
|---|---:|---:|---:|
| lazy_task | 17.21 | 17.21-17.86 | 5 |
| await_chain | 88.16 | 87.79-88.79 | 5 |
| empty_poll | 114.14 | 113.52-114.54 | 5 |
| yield | 36.65 | 35.75-37.73 | 5 |
| post | 48.23 | 47.79-50.63 | 5 |
| cross_thread_post | 71.02 | 70.99-78.16 | 5 |
| spawn_completion | 69.13 | 68.03-70.44 | 5 |
| zero_timer | 68.47 | 68.11-70.60 | 5 |

Each op has the component-specific definition saved in the JSON report.
These are repeated batch averages, not per-operation latency percentiles.
