| Component | Median ns/op | Min-max ns/op | Trials |
|---|---:|---:|---:|
| lazy_task | 33.85 | 28.97-34.60 | 5 |
| await_chain | 112.97 | 102.42-126.39 | 5 |
| empty_poll | 24289.56 | 22580.09-27563.22 | 5 |
| yield | 33.69 | 29.85-35.69 | 5 |
| post | 38.54 | 36.00-43.15 | 5 |
| cross_thread_post | 24.11 | 22.56-29.25 | 5 |
| spawn_completion | 88.83 | 81.55-95.92 | 5 |
| zero_timer | 69.87 | 61.41-73.64 | 5 |

Each op has the component-specific definition saved in the JSON report.
These are repeated batch averages, not per-operation latency percentiles.
