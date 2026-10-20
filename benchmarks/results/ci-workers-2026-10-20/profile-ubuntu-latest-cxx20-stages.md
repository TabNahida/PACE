# PACE stage timing diagnostic

**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**

3 trials. Values below are medians across trials; ranges show exclusive-time spread.

| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |
|---|---:|---:|---:|---:|---:|---:|
| send | 44,000.0 | 316.011 | 316.011 | 313.459-329.152 | 73.06% | 7,182.1 |
| recv | 87,991.0 | 65.318 | 65.318 | 65.203-67.163 | 15.10% | 742.3 |
| poller_update | 43,989.0 | 20.250 | 20.250 | 20.232-20.983 | 4.68% | 460.3 |
| poll_blocking | 15,836.0 | 12.631 | 12.631 | 12.356-12.728 | 2.86% | 797.6 |
| coroutine_execution | 15,838.0 | 414.917 | 10.038 | 9.997-10.289 | 2.32% | 26,113.5 |
| io_arm | 43,989.0 | 23.517 | 3.267 | 3.267-3.431 | 0.76% | 534.5 |
| reactor_run | 1.0 | 432.536 | 2.658 | 2.590-2.698 | 0.61% | 432,535,660.0 |
| timers | 15,837.0 | 0.698 | 0.698 | 0.693-0.709 | 0.16% | 44.8 |
| event_dispatch | 15,837.0 | 0.610 | 0.610 | 0.601-0.646 | 0.14% | 39.2 |
| root_reaping | 15,838.0 | 0.458 | 0.458 | 0.455-0.459 | 0.11% | 29.0 |
| inbox | 15,838.0 | 0.415 | 0.415 | 0.397-0.425 | 0.10% | 26.1 |
| post_dispatch | 15,838.0 | 0.386 | 0.386 | 0.362-0.387 | 0.09% | 24.3 |
| accept | 6.0 | 0.029 | 0.029 | 0.028-0.033 | 0.01% | 4,714.3 |
| poll_nonblocking | 1.0 | 0.001 | 0.001 | 0.000-0.001 | 0.00% | 501.0 |

`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.

- Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv
- Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency
- The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup
- Listener construction and initial spawn on the main thread precede the worker reset and are excluded
- Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime
- Per-stage medians and median shares need not sum to a median total or 100 percent
- Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile
