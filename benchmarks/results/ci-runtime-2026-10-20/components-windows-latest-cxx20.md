| Component | Median ns/op | Min-max ns/op | Trials |
|---|---:|---:|---:|
| lazy_task | 44.76 | 43.59-77.40 | 5 |
| await_chain | 218.41 | 212.62-221.05 | 5 |
| empty_poll | 1347.78 | 1321.71-1433.38 | 5 |
| yield | 40.55 | 39.94-42.12 | 5 |
| post | 43.56 | 41.61-58.84 | 5 |
| cross_thread_post | 199.35 | 190.97-215.51 | 5 |
| spawn_completion | 143.29 | 134.99-216.13 | 5 |
| zero_timer | 65.66 | 64.69-84.12 | 5 |

Each op has the component-specific definition saved in the JSON report.
These are repeated batch averages, not per-operation latency percentiles.
