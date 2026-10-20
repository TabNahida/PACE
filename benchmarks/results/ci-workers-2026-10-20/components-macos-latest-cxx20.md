| Component | Median ns/op | Min-max ns/op | Trials |
|---|---:|---:|---:|
| lazy_task | 31.67 | 31.50-75.32 | 5 |
| await_chain | 147.10 | 115.30-171.14 | 5 |
| empty_poll | 22625.54 | 22540.51-24539.03 | 5 |
| yield | 33.99 | 32.53-49.36 | 5 |
| post | 40.11 | 36.31-76.59 | 5 |
| cross_thread_post | 36.53 | 31.52-38.90 | 5 |
| spawn_completion | 114.46 | 87.03-187.67 | 5 |
| zero_timer | 89.54 | 83.42-108.30 | 5 |

Each op has the component-specific definition saved in the JSON report.
These are repeated batch averages, not per-operation latency percentiles.
