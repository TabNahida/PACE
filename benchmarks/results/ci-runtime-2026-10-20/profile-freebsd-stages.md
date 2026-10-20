# PACE stage timing diagnostic

**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**

3 trials. Values below are medians across trials; ranges show exclusive-time spread.

| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |
|---|---:|---:|---:|---:|---:|---:|
| send | 44,000.0 | 54.358 | 54.358 | 54.161-54.616 | 34.00% | 1,235.4 |
| recv | 87,999.0 | 32.137 | 32.137 | 31.911-32.282 | 19.86% | 365.2 |
| poll_blocking | 22,363.0 | 18.279 | 18.279 | 16.680-21.759 | 11.38% | 817.4 |
| coroutine_execution | 22,366.0 | 127.434 | 17.882 | 17.740-18.500 | 11.14% | 5,697.7 |
| poller_update | 43,997.0 | 16.874 | 16.874 | 16.802-17.205 | 10.46% | 383.5 |
| reactor_run | 1.0 | 160.651 | 7.879 | 7.866-8.295 | 4.96% | 160,651,375.0 |
| io_arm | 43,997.0 | 23.098 | 6.224 | 6.215-6.299 | 3.87% | 525.0 |
| timers | 22,365.0 | 1.885 | 1.885 | 1.832-1.967 | 1.17% | 82.6 |
| event_dispatch | 22,365.0 | 1.882 | 1.882 | 1.855-1.991 | 1.19% | 83.6 |
| root_reaping | 22,366.0 | 1.234 | 1.234 | 1.111-1.266 | 0.76% | 53.2 |
| inbox | 22,366.0 | 1.133 | 1.133 | 1.105-1.158 | 0.70% | 49.7 |
| post_dispatch | 22,366.0 | 1.040 | 1.040 | 0.976-1.060 | 0.63% | 43.9 |
| accept | 6.0 | 0.007 | 0.007 | 0.007-0.007 | 0.00% | 1,123.3 |
| poll_nonblocking | 2.0 | 0.003 | 0.003 | 0.003-0.005 | 0.00% | 1,562.5 |

`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.

- Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv
- Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency
- The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup
- Listener construction and initial spawn on the main thread precede the worker reset and are excluded
- Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime
- Per-stage medians and median shares need not sum to a median total or 100 percent
- Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile
