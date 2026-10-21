| Component | Median ns/op | Min-max ns/op | Trials |
|---|---:|---:|---:|
| lazy_task | 32.38 | 31.09-55.50 | 5 |
| await_chain | 126.00 | 115.03-168.39 | 5 |
| empty_poll | 32457.29 | 26279.72-36817.26 | 5 |
| yield | 36.97 | 35.73-40.67 | 5 |
| post | 39.72 | 36.42-66.64 | 5 |
| cross_thread_post | 45.94 | 23.67-63.39 | 5 |
| spawn_completion | 138.06 | 90.43-147.45 | 5 |
| zero_timer | 74.28 | 68.40-107.50 | 5 |

Each op has the component-specific definition saved in the JSON report.
These are repeated batch averages, not per-operation latency percentiles.
