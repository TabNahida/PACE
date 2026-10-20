# PACE stage timing diagnostic

**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**

3 trials. Values below are medians across trials; ranges show exclusive-time spread.

| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |
|---|---:|---:|---:|---:|---:|---:|
| send | 84,000.0 | 228.754 | 228.754 | 223.006-234.944 | 43.06% | 2,723.3 |
| poll_blocking | 45,469.0 | 198.210 | 198.210 | 170.203-209.197 | 37.31% | 4,359.2 |
| recv | 167,983.0 | 46.692 | 46.692 | 46.448-48.075 | 8.74% | 278.0 |
| coroutine_execution | 45,471.0 | 315.011 | 20.509 | 18.356-21.685 | 3.86% | 6,927.7 |
| poller_update | 83,981.0 | 14.823 | 14.823 | 14.262-14.928 | 2.79% | 176.5 |
| reactor_run | 1.0 | 531.195 | 9.336 | 8.992-9.975 | 1.69% | 531,195,042.0 |
| io_arm | 83,981.0 | 19.287 | 4.575 | 4.464-4.629 | 0.84% | 229.7 |
| inbox | 45,471.0 | 3.982 | 3.982 | 3.816-4.060 | 0.74% | 83.9 |
| event_dispatch | 45,470.0 | 2.205 | 2.205 | 1.971-2.622 | 0.44% | 43.4 |
| timers | 45,470.0 | 1.659 | 1.659 | 1.585-1.794 | 0.30% | 34.9 |
| root_reaping | 45,471.0 | 0.816 | 0.816 | 0.781-0.891 | 0.15% | 17.6 |
| post_dispatch | 45,471.0 | 0.804 | 0.804 | 0.793-0.877 | 0.15% | 17.4 |
| accept | 6.0 | 0.019 | 0.019 | 0.013-0.020 | 0.00% | 2,811.7 |
| poll_nonblocking | 1.0 | 0.000 | 0.000 | 0.000-0.001 | 0.00% | 387.0 |

`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.

- Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv
- Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency
- The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup
- Listener construction and initial spawn on the main thread precede the worker reset and are excluded
- Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime
- Per-stage medians and median shares need not sum to a median total or 100 percent
- Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile
