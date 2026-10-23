# PACE stage timing diagnostic

**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**

3 trials. Values below are medians across trials; ranges show exclusive-time spread.

| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |
|---|---:|---:|---:|---:|---:|---:|
| send | 352,000.0 | 829.990 | 829.990 | 829.867-834.927 | 74.58% | 2,357.9 |
| recv | 352,032.0 | 138.770 | 138.770 | 138.757-140.629 | 12.47% | 394.2 |
| poll_blocking | 15,250.0 | 48.261 | 48.261 | 45.102-52.721 | 4.31% | 3,122.7 |
| coroutine_execution | 15,254.0 | 1060.920 | 39.786 | 39.682-39.971 | 3.56% | 69,394.0 |
| accept | 36.0 | 38.361 | 38.361 | 29.130-42.701 | 3.45% | 1,065,573.2 |
| io_arm | 352,004.0 | 13.965 | 13.939 | 13.602-14.036 | 1.25% | 39.7 |
| reactor_run | 1.0 | 1118.761 | 2.992 | 2.992-3.386 | 0.27% | 1,118,761,388.0 |
| event_dispatch | 15,253.0 | 2.120 | 2.120 | 2.015-2.492 | 0.19% | 137.2 |
| root_reaping | 15,254.0 | 0.555 | 0.555 | 0.554-0.565 | 0.05% | 37.0 |
| timers | 15,253.0 | 0.519 | 0.519 | 0.518-0.524 | 0.05% | 34.0 |
| inbox | 15,254.0 | 0.277 | 0.277 | 0.276-0.282 | 0.02% | 18.1 |
| post_dispatch | 15,254.0 | 0.259 | 0.259 | 0.259-0.272 | 0.02% | 17.0 |
| poller_update | 33.0 | 0.026 | 0.026 | 0.025-0.027 | 0.00% | 792.5 |
| poll_nonblocking | 3.0 | 0.007 | 0.007 | 0.005-0.007 | 0.00% | 2,174.0 |

`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.

- Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv
- Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency
- The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup
- Listener construction and initial spawn on the main thread precede the worker reset and are excluded
- Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime
- Per-stage medians and median shares need not sum to a median total or 100 percent
- Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile
