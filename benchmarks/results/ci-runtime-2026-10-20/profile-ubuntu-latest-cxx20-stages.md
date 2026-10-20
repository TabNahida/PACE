# PACE stage timing diagnostic

**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**

3 trials. Values below are medians across trials; ranges show exclusive-time spread.

| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |
|---|---:|---:|---:|---:|---:|---:|
| send | 44,000.0 | 304.141 | 304.141 | 291.880-316.897 | 64.99% | 6,912.3 |
| recv | 87,989.0 | 66.949 | 66.949 | 64.544-67.251 | 13.86% | 761.1 |
| coroutine_execution | 17,593.0 | 437.976 | 39.706 | 39.225-41.740 | 8.38% | 24,894.9 |
| poller_update | 43,987.0 | 24.155 | 24.155 | 23.504-25.470 | 5.16% | 549.0 |
| poll_blocking | 17,589.0 | 21.183 | 21.183 | 20.467-21.598 | 4.53% | 1,165.4 |
| io_arm | 43,987.0 | 30.030 | 5.875 | 5.440-6.585 | 1.26% | 682.5 |
| reactor_run | 1.0 | 467.973 | 4.512 | 4.475-5.117 | 0.96% | 467,972,945.0 |
| event_dispatch | 17,592.0 | 1.276 | 1.276 | 1.200-1.310 | 0.27% | 68.3 |
| timers | 17,592.0 | 1.139 | 1.139 | 1.138-1.290 | 0.24% | 64.8 |
| root_reaping | 17,593.0 | 0.704 | 0.704 | 0.684-0.776 | 0.15% | 39.3 |
| inbox | 17,593.0 | 0.664 | 0.664 | 0.656-0.713 | 0.14% | 37.4 |
| post_dispatch | 17,593.0 | 0.576 | 0.576 | 0.574-0.630 | 0.12% | 32.7 |
| accept | 7.0 | 0.035 | 0.035 | 0.032-0.036 | 0.01% | 4,583.1 |
| poll_nonblocking | 2.0 | 0.001 | 0.001 | 0.001-0.002 | 0.00% | 581.3 |

`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.

- Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv
- Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency
- The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup
- Listener construction and initial spawn on the main thread precede the worker reset and are excluded
- Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime
- Per-stage medians and median shares need not sum to a median total or 100 percent
- Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile
