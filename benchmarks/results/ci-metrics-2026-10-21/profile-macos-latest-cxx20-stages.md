# PACE stage timing diagnostic

**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**

3 trials. Values below are medians across trials; ranges show exclusive-time spread.

| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |
|---|---:|---:|---:|---:|---:|---:|
| poll_blocking | 24,162.0 | 356.121 | 356.121 | 272.803-449.405 | 41.17% | 14,523.1 |
| recv | 87,934.0 | 227.116 | 227.116 | 220.428-273.204 | 26.26% | 2,581.4 |
| send | 44,000.0 | 176.600 | 176.600 | 171.247-208.786 | 20.42% | 4,013.6 |
| poller_update | 43,933.0 | 41.912 | 41.912 | 41.598-54.896 | 5.19% | 954.0 |
| coroutine_execution | 24,165.0 | 487.695 | 35.000 | 32.753-39.697 | 4.05% | 20,899.6 |
| reactor_run | 1.0 | 864.944 | 10.784 | 9.762-11.767 | 1.25% | 864,944,250.0 |
| io_arm | 43,933.0 | 49.665 | 7.753 | 7.364-8.553 | 0.85% | 1,130.5 |
| event_dispatch | 24,163.0 | 3.489 | 3.489 | 2.933-4.061 | 0.38% | 142.3 |
| timers | 24,163.0 | 2.577 | 2.577 | 2.307-2.715 | 0.30% | 105.1 |
| root_reaping | 24,165.0 | 1.835 | 1.835 | 1.516-1.919 | 0.20% | 74.8 |
| inbox | 24,165.0 | 1.324 | 1.324 | 1.214-1.532 | 0.15% | 54.0 |
| post_dispatch | 24,165.0 | 1.100 | 1.100 | 1.078-1.445 | 0.14% | 47.5 |
| poll_nonblocking | 1.0 | 0.031 | 0.031 | 0.021-0.041 | 0.00% | 21,375.0 |
| accept | 6.0 | 0.016 | 0.016 | 0.013-0.035 | 0.00% | 2,646.0 |

`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.

- Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv
- Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency
- The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup
- Listener construction and initial spawn on the main thread precede the worker reset and are excluded
- Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime
- Per-stage medians and median shares need not sum to a median total or 100 percent
- Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile
