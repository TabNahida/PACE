# PACE stage timing diagnostic

**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**

3 trials. Values below are medians across trials; ranges show exclusive-time spread.

| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |
|---|---:|---:|---:|---:|---:|---:|
| send | 352,000.0 | 827.263 | 827.263 | 824.726-837.219 | 71.71% | 2,350.2 |
| recv | 703,993.0 | 180.725 | 180.725 | 180.491-181.044 | 15.52% | 256.7 |
| poll_blocking | 15,111.0 | 50.506 | 50.506 | 47.380-61.213 | 4.40% | 3,386.2 |
| coroutine_execution | 15,116.0 | 1096.314 | 44.842 | 44.214-45.095 | 3.86% | 73,121.3 |
| accept | 37.0 | 32.332 | 32.332 | 24.559-36.266 | 2.78% | 850,830.4 |
| io_arm | 351,966.0 | 13.981 | 13.947 | 13.644-14.288 | 1.19% | 39.7 |
| reactor_run | 1.0 | 1164.153 | 2.867 | 2.818-3.019 | 0.25% | 1,164,152,963.0 |
| event_dispatch | 15,115.0 | 2.046 | 2.046 | 1.934-2.590 | 0.18% | 135.3 |
| root_reaping | 15,116.0 | 0.533 | 0.533 | 0.523-0.564 | 0.05% | 35.6 |
| timers | 15,115.0 | 0.510 | 0.510 | 0.502-0.532 | 0.04% | 33.7 |
| inbox | 15,116.0 | 0.298 | 0.298 | 0.289-0.301 | 0.03% | 19.3 |
| post_dispatch | 15,116.0 | 0.250 | 0.250 | 0.249-0.267 | 0.02% | 16.8 |
| poller_update | 33.0 | 0.030 | 0.030 | 0.025-0.034 | 0.00% | 896.6 |
| poll_nonblocking | 4.0 | 0.009 | 0.009 | 0.003-0.009 | 0.00% | 1,724.4 |

`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.

- Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv
- Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency
- The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup
- Listener construction and initial spawn on the main thread precede the worker reset and are excluded
- Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime
- Per-stage medians and median shares need not sum to a median total or 100 percent
- Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile
