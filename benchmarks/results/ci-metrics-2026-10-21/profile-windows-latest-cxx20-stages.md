# PACE stage timing diagnostic

**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**

3 trials. Values below are medians across trials; ranges show exclusive-time spread.

| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |
|---|---:|---:|---:|---:|---:|---:|
| send | 44,000.0 | 912.101 | 912.101 | 896.342-923.023 | 75.50% | 20,729.6 |
| recv | 86,493.0 | 129.315 | 129.315 | 127.679-129.589 | 10.73% | 1,498.3 |
| poll_blocking | 11,655.0 | 76.113 | 76.113 | 75.384-79.351 | 6.35% | 6,610.5 |
| coroutine_execution | 11,658.0 | 1118.011 | 52.583 | 51.526-53.602 | 4.35% | 95,744.4 |
| io_arm | 42,491.0 | 22.857 | 11.958 | 11.578-12.002 | 0.99% | 534.9 |
| poller_update | 42,491.0 | 10.972 | 10.972 | 10.899-11.538 | 0.92% | 260.1 |
| reactor_run | 1.0 | 1208.050 | 6.931 | 6.869-7.327 | 0.58% | 1,208,049,900.0 |
| event_dispatch | 11,657.0 | 2.700 | 2.700 | 2.610-2.765 | 0.22% | 229.1 |
| timers | 11,657.0 | 1.278 | 1.278 | 1.226-1.346 | 0.11% | 111.0 |
| root_reaping | 11,658.0 | 1.230 | 1.230 | 1.093-1.262 | 0.10% | 106.8 |
| inbox | 11,658.0 | 0.924 | 0.924 | 0.920-0.934 | 0.08% | 79.9 |
| post_dispatch | 11,658.0 | 0.831 | 0.831 | 0.814-0.861 | 0.07% | 70.5 |
| accept | 7.0 | 0.198 | 0.198 | 0.145-0.205 | 0.02% | 29,285.7 |
| poll_nonblocking | 2.0 | 0.012 | 0.012 | 0.004-0.013 | 0.00% | 6,050.0 |

`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.

- Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv
- Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency
- The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup
- Listener construction and initial spawn on the main thread precede the worker reset and are excluded
- Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime
- Per-stage medians and median shares need not sum to a median total or 100 percent
- Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile
