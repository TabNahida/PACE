| Component | Median ns/op | Min-max ns/op | Trials |
|---|---:|---:|---:|
| lazy_task | 46.63 | 44.99-81.08 | 5 |
| await_chain | 235.50 | 227.99-323.73 | 5 |
| empty_poll | 1569.14 | 1492.43-1883.84 | 5 |
| yield | 36.44 | 35.29-50.00 | 5 |
| post | 44.02 | 43.47-68.44 | 5 |
| cross_thread_post | 242.04 | 146.09-263.77 | 5 |
| spawn_completion | 170.00 | 146.21-218.12 | 5 |
| zero_timer | 68.35 | 66.95-90.16 | 5 |

Each op has the component-specific definition saved in the JSON report.
These are repeated batch averages, not per-operation latency percentiles.
