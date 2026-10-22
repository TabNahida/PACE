# PACE stage timing diagnostic

**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**

3 trials. Values below are medians across trials; ranges show exclusive-time spread.

| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |
|---|---:|---:|---:|---:|---:|---:|
| send | 44,000.0 | 274.329 | 274.329 | 268.835-290.539 | 66.28% | 6,234.7 |
| recv | 87,996.0 | 57.982 | 57.982 | 56.935-58.804 | 13.76% | 658.9 |
| coroutine_execution | 17,085.0 | 389.550 | 37.149 | 35.734-38.588 | 8.63% | 23,289.2 |
| poll_blocking | 17,083.0 | 18.067 | 18.067 | 17.121-18.685 | 4.29% | 1,057.6 |
| poller_update | 43,996.0 | 18.058 | 18.058 | 17.031-18.272 | 4.20% | 410.5 |
| io_arm | 43,996.0 | 22.518 | 4.569 | 4.460-4.862 | 1.11% | 511.9 |
| reactor_run | 1.0 | 413.876 | 3.225 | 3.101-3.650 | 0.78% | 413,876,078.0 |
| event_dispatch | 17,084.0 | 0.843 | 0.843 | 0.793-0.982 | 0.20% | 49.3 |
| timers | 17,084.0 | 0.823 | 0.823 | 0.791-0.905 | 0.20% | 48.2 |
| root_reaping | 17,085.0 | 0.498 | 0.498 | 0.484-0.545 | 0.12% | 29.3 |
| inbox | 17,085.0 | 0.479 | 0.479 | 0.448-0.539 | 0.12% | 28.0 |
| post_dispatch | 17,085.0 | 0.390 | 0.390 | 0.371-0.434 | 0.09% | 22.8 |
| accept | 6.0 | 0.031 | 0.031 | 0.028-0.035 | 0.01% | 4,734.3 |
| poll_nonblocking | 1.0 | 0.001 | 0.001 | 0.001-0.002 | 0.00% | 721.3 |

`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.

- Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv
- Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency
- The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup
- Listener construction and initial spawn on the main thread precede the worker reset and are excluded
- Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime
- Per-stage medians and median shares need not sum to a median total or 100 percent
- Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile
