# PACE stage timing diagnostic

**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**

3 trials. Values below are medians across trials; ranges show exclusive-time spread.

| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |
|---|---:|---:|---:|---:|---:|---:|
| poll_blocking | 24,208.0 | 306.221 | 306.221 | 299.047-320.285 | 37.96% | 12,649.6 |
| recv | 87,969.0 | 222.660 | 222.660 | 218.042-246.113 | 27.60% | 2,531.7 |
| send | 44,000.0 | 174.854 | 174.854 | 170.640-195.840 | 21.68% | 3,973.9 |
| poller_update | 43,967.0 | 41.359 | 41.359 | 39.637-43.007 | 5.08% | 941.1 |
| coroutine_execution | 24,211.0 | 481.048 | 34.242 | 30.079-35.548 | 4.20% | 19,869.0 |
| reactor_run | 1.0 | 806.679 | 10.123 | 9.984-10.170 | 1.24% | 806,678,959.0 |
| io_arm | 43,967.0 | 49.266 | 7.527 | 6.924-7.907 | 0.89% | 1,121.1 |
| event_dispatch | 24,209.0 | 3.016 | 3.016 | 2.888-3.437 | 0.37% | 124.6 |
| timers | 24,209.0 | 2.317 | 2.317 | 2.286-2.402 | 0.28% | 94.4 |
| root_reaping | 24,211.0 | 1.454 | 1.454 | 1.407-1.580 | 0.18% | 60.1 |
| inbox | 24,211.0 | 1.279 | 1.279 | 1.258-1.377 | 0.16% | 52.8 |
| post_dispatch | 24,211.0 | 1.141 | 1.141 | 1.033-1.232 | 0.14% | 45.8 |
| poll_nonblocking | 1.0 | 0.067 | 0.067 | 0.021-0.175 | 0.01% | 33,479.0 |
| accept | 6.0 | 0.022 | 0.022 | 0.017-0.026 | 0.00% | 3,190.3 |

`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.

- Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv
- Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency
- The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup
- Listener construction and initial spawn on the main thread precede the worker reset and are excluded
- Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime
- Per-stage medians and median shares need not sum to a median total or 100 percent
- Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile
