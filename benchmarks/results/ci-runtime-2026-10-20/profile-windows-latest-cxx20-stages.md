# PACE stage timing diagnostic

**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**

3 trials. Values below are medians across trials; ranges show exclusive-time spread.

| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |
|---|---:|---:|---:|---:|---:|---:|
| send | 44,000.0 | 626.725 | 626.725 | 626.324-645.652 | 70.09% | 14,243.7 |
| recv | 87,211.0 | 108.940 | 108.940 | 104.148-110.309 | 11.97% | 1,249.2 |
| poll_blocking | 13,691.0 | 73.979 | 73.979 | 66.196-75.304 | 8.17% | 5,343.7 |
| coroutine_execution | 13,694.0 | 806.276 | 50.998 | 50.840-51.462 | 5.65% | 58,509.3 |
| io_arm | 43,208.0 | 19.665 | 11.014 | 10.914-11.071 | 1.22% | 455.1 |
| reactor_run | 1.0 | 899.850 | 9.802 | 9.684-10.270 | 1.09% | 899,849,900.0 |
| poller_update | 43,208.0 | 8.686 | 8.686 | 8.651-8.850 | 0.96% | 200.3 |
| event_dispatch | 13,692.0 | 2.627 | 2.627 | 2.521-2.774 | 0.30% | 191.9 |
| timers | 13,692.0 | 1.772 | 1.772 | 1.763-1.823 | 0.20% | 129.4 |
| root_reaping | 13,694.0 | 1.532 | 1.532 | 1.517-1.623 | 0.17% | 113.2 |
| post_dispatch | 13,694.0 | 1.455 | 1.455 | 1.387-1.724 | 0.16% | 106.3 |
| inbox | 13,694.0 | 1.426 | 1.426 | 1.414-1.431 | 0.16% | 104.1 |
| accept | 6.0 | 0.115 | 0.115 | 0.107-0.156 | 0.01% | 21,360.0 |
| poll_nonblocking | 1.0 | 0.009 | 0.009 | 0.004-0.010 | 0.00% | 4,550.0 |

`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.

- Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv
- Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency
- The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup
- Listener construction and initial spawn on the main thread precede the worker reset and are excluded
- Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime
- Per-stage medians and median shares need not sum to a median total or 100 percent
- Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile
