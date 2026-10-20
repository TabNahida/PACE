# PACE stage timing diagnostic

**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**

3 trials. Values below are medians across trials; ranges show exclusive-time spread.

| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |
|---|---:|---:|---:|---:|---:|---:|
| send | 44,000.0 | 915.393 | 915.393 | 906.895-925.402 | 75.49% | 20,804.4 |
| recv | 86,025.0 | 127.169 | 127.169 | 125.932-127.173 | 10.46% | 1,478.3 |
| poll_blocking | 11,466.0 | 79.734 | 79.734 | 78.988-81.512 | 6.58% | 6,954.0 |
| coroutine_execution | 11,468.0 | 1119.316 | 53.602 | 52.871-54.053 | 4.41% | 96,847.0 |
| io_arm | 42,022.0 | 22.209 | 11.095 | 11.023-11.365 | 0.92% | 528.5 |
| poller_update | 42,022.0 | 10.963 | 10.963 | 10.844-11.879 | 0.91% | 259.0 |
| reactor_run | 1.0 | 1215.467 | 7.125 | 7.061-7.146 | 0.59% | 1,215,466,900.0 |
| event_dispatch | 11,467.0 | 3.045 | 3.045 | 3.043-3.121 | 0.25% | 266.1 |
| timers | 11,467.0 | 1.221 | 1.221 | 1.207-1.229 | 0.10% | 105.6 |
| root_reaping | 11,468.0 | 1.156 | 1.156 | 1.130-1.159 | 0.09% | 99.3 |
| post_dispatch | 11,468.0 | 1.026 | 1.026 | 1.026-1.034 | 0.09% | 89.5 |
| inbox | 11,468.0 | 1.020 | 1.020 | 0.947-1.050 | 0.08% | 89.0 |
| accept | 5.0 | 0.149 | 0.149 | 0.119-0.178 | 0.01% | 24,800.0 |
| poll_nonblocking | 1.0 | 0.009 | 0.009 | 0.003-0.009 | 0.00% | 8,500.0 |

`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.

- Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv
- Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency
- The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup
- Listener construction and initial spawn on the main thread precede the worker reset and are excluded
- Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime
- Per-stage medians and median shares need not sum to a median total or 100 percent
- Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile
