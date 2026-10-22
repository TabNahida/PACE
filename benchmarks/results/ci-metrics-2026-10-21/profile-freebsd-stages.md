# PACE stage timing diagnostic

**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**

3 trials. Values below are medians across trials; ranges show exclusive-time spread.

| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |
|---|---:|---:|---:|---:|---:|---:|
| send | 44,000.0 | 52.628 | 52.628 | 52.599-53.958 | 33.51% | 1,196.1 |
| recv | 87,993.0 | 32.206 | 32.206 | 31.886-32.295 | 20.50% | 366.0 |
| coroutine_execution | 22,188.0 | 126.519 | 18.179 | 17.974-18.361 | 11.57% | 5,698.9 |
| poller_update | 43,990.0 | 17.009 | 17.009 | 16.751-17.190 | 10.94% | 386.6 |
| poll_blocking | 22,186.0 | 15.974 | 15.974 | 13.974-20.123 | 10.17% | 720.0 |
| reactor_run | 1.0 | 157.070 | 7.651 | 7.642-7.819 | 4.87% | 157,069,938.0 |
| io_arm | 43,990.0 | 23.144 | 6.136 | 5.941-6.309 | 3.97% | 526.1 |
| event_dispatch | 22,187.0 | 1.873 | 1.873 | 1.864-1.913 | 1.19% | 84.0 |
| timers | 22,187.0 | 1.854 | 1.854 | 1.818-1.869 | 1.18% | 83.1 |
| inbox | 22,188.0 | 1.104 | 1.104 | 1.067-1.157 | 0.71% | 49.9 |
| root_reaping | 22,188.0 | 1.103 | 1.103 | 1.092-1.134 | 0.71% | 49.4 |
| post_dispatch | 22,188.0 | 0.991 | 0.991 | 0.984-0.993 | 0.63% | 44.7 |
| accept | 6.0 | 0.007 | 0.007 | 0.007-0.007 | 0.00% | 1,236.8 |
| poll_nonblocking | 1.0 | 0.002 | 0.002 | 0.000-0.003 | 0.00% | 1,267.0 |

`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.

- Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv
- Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency
- The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup
- Listener construction and initial spawn on the main thread precede the worker reset and are excluded
- Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime
- Per-stage medians and median shares need not sum to a median total or 100 percent
- Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile
