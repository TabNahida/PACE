# PACE stage timing diagnostic

**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**

3 trials. Values below are medians across trials; ranges show exclusive-time spread.

| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |
|---|---:|---:|---:|---:|---:|---:|
| send | 44,000.0 | 78.192 | 78.192 | 77.341-79.112 | 34.03% | 1,777.1 |
| recv | 87,982.0 | 45.491 | 45.491 | 45.408-45.551 | 19.76% | 517.1 |
| coroutine_execution | 22,117.0 | 185.542 | 28.776 | 28.239-29.172 | 12.52% | 8,389.1 |
| poller_update | 43,979.0 | 24.987 | 24.987 | 24.316-25.079 | 10.91% | 568.1 |
| poll_blocking | 22,114.0 | 23.501 | 23.501 | 19.672-27.767 | 10.23% | 1,056.0 |
| reactor_run | 1.0 | 229.778 | 10.199 | 10.070-10.562 | 4.48% | 229,778,060.0 |
| io_arm | 43,979.0 | 33.435 | 8.448 | 8.375-8.531 | 3.71% | 760.1 |
| event_dispatch | 22,115.0 | 2.674 | 2.674 | 2.631-2.714 | 1.17% | 120.9 |
| timers | 22,115.0 | 2.567 | 2.567 | 2.536-2.593 | 1.12% | 115.3 |
| root_reaping | 22,117.0 | 1.518 | 1.518 | 1.504-1.595 | 0.67% | 68.8 |
| inbox | 22,117.0 | 1.497 | 1.497 | 1.478-1.583 | 0.64% | 67.7 |
| post_dispatch | 22,117.0 | 1.315 | 1.315 | 1.301-1.365 | 0.58% | 59.5 |
| accept | 5.0 | 0.008 | 0.008 | 0.008-0.008 | 0.00% | 1,546.8 |
| poll_nonblocking | 1.0 | 0.003 | 0.003 | 0.003-0.004 | 0.00% | 2,785.0 |

`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.

- Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv
- Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency
- The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup
- Listener construction and initial spawn on the main thread precede the worker reset and are excluded
- Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime
- Per-stage medians and median shares need not sum to a median total or 100 percent
- Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile
