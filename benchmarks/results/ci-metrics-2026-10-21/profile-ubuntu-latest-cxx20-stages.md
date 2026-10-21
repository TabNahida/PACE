# PACE stage timing diagnostic

**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**

3 trials. Values below are medians across trials; ranges show exclusive-time spread.

| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |
|---|---:|---:|---:|---:|---:|---:|
| send | 44,000.0 | 438.588 | 438.588 | 435.885-477.243 | 67.64% | 9,967.9 |
| recv | 87,993.0 | 116.126 | 116.126 | 115.907-119.399 | 17.85% | 1,319.7 |
| poller_update | 43,991.0 | 41.044 | 41.044 | 39.851-41.669 | 6.12% | 932.9 |
| poll_blocking | 16,107.0 | 24.719 | 24.719 | 20.600-33.450 | 3.80% | 1,534.7 |
| coroutine_execution | 16,109.0 | 617.371 | 16.959 | 16.820-18.273 | 2.61% | 39,486.7 |
| io_arm | 43,991.0 | 46.896 | 5.853 | 5.807-6.029 | 0.89% | 1,065.9 |
| reactor_run | 1.0 | 650.663 | 4.400 | 4.141-4.544 | 0.64% | 650,662,558.0 |
| event_dispatch | 16,108.0 | 1.196 | 1.196 | 1.151-1.282 | 0.18% | 76.7 |
| timers | 16,108.0 | 1.121 | 1.121 | 1.070-1.322 | 0.17% | 69.6 |
| root_reaping | 16,109.0 | 0.713 | 0.713 | 0.672-0.733 | 0.10% | 43.9 |
| inbox | 16,109.0 | 0.620 | 0.620 | 0.607-0.657 | 0.09% | 39.3 |
| post_dispatch | 16,109.0 | 0.579 | 0.579 | 0.552-0.642 | 0.09% | 36.0 |
| accept | 7.0 | 0.044 | 0.044 | 0.040-0.050 | 0.01% | 6,247.7 |
| poll_nonblocking | 2.0 | 0.002 | 0.002 | 0.001-0.004 | 0.00% | 1,131.5 |

`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.

- Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv
- Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency
- The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup
- Listener construction and initial spawn on the main thread precede the worker reset and are excluded
- Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime
- Per-stage medians and median shares need not sum to a median total or 100 percent
- Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile
