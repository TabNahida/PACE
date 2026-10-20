# PACE stage timing diagnostic

**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**

3 trials. Values below are medians across trials; ranges show exclusive-time spread.

| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |
|---|---:|---:|---:|---:|---:|---:|
| poll_blocking | 24,248.0 | 349.544 | 349.544 | 328.237-360.301 | 38.80% | 14,174.0 |
| recv | 87,970.0 | 240.086 | 240.086 | 233.461-262.407 | 27.60% | 2,728.4 |
| send | 44,000.0 | 198.204 | 198.204 | 185.656-210.079 | 22.36% | 4,504.6 |
| poller_update | 43,970.0 | 42.883 | 42.883 | 41.746-44.900 | 4.78% | 975.3 |
| coroutine_execution | 24,251.0 | 517.547 | 30.753 | 29.937-33.211 | 3.54% | 21,266.9 |
| reactor_run | 1.0 | 886.442 | 10.031 | 9.664-10.394 | 1.13% | 886,442,125.0 |
| io_arm | 43,970.0 | 49.977 | 7.094 | 6.745-7.685 | 0.82% | 1,136.6 |
| event_dispatch | 24,250.0 | 3.005 | 3.005 | 2.681-3.232 | 0.34% | 121.9 |
| timers | 24,250.0 | 2.385 | 2.385 | 2.302-2.451 | 0.26% | 101.1 |
| root_reaping | 24,251.0 | 1.524 | 1.524 | 1.509-1.540 | 0.17% | 63.5 |
| inbox | 24,251.0 | 1.309 | 1.309 | 1.226-1.329 | 0.14% | 53.9 |
| post_dispatch | 24,251.0 | 1.137 | 1.137 | 1.103-1.138 | 0.13% | 46.9 |
| poll_nonblocking | 2.0 | 0.051 | 0.051 | 0.022-0.141 | 0.01% | 25,437.0 |
| accept | 7.0 | 0.019 | 0.019 | 0.012-0.030 | 0.00% | 2,369.8 |

`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.

- Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv
- Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency
- The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup
- Listener construction and initial spawn on the main thread precede the worker reset and are excluded
- Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime
- Per-stage medians and median shares need not sum to a median total or 100 percent
- Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile
