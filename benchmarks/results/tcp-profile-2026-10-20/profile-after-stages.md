# PACE stage timing diagnostic

**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**

3 trials. Values below are medians across trials; ranges show exclusive-time spread.

| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |
|---|---:|---:|---:|---:|---:|---:|
| send | 84,000.0 | 233.457 | 233.457 | 215.964-244.353 | 45.68% | 2,779.3 |
| poll_blocking | 50,665.0 | 177.351 | 177.351 | 93.619-231.435 | 34.70% | 3,500.5 |
| recv | 167,987.0 | 47.564 | 47.564 | 44.584-49.419 | 9.31% | 283.1 |
| coroutine_execution | 50,668.0 | 320.722 | 20.446 | 15.791-23.712 | 4.00% | 6,329.9 |
| poller_update | 83,986.0 | 14.796 | 14.796 | 14.553-15.177 | 2.90% | 176.2 |
| reactor_run | 1.0 | 511.057 | 6.428 | 5.884-7.455 | 1.26% | 511,057,042.0 |
| io_arm | 83,986.0 | 19.242 | 4.445 | 4.309-4.629 | 0.87% | 229.1 |
| event_dispatch | 50,667.0 | 2.306 | 2.306 | 2.034-2.707 | 0.47% | 45.5 |
| timers | 50,667.0 | 1.641 | 1.641 | 1.424-1.965 | 0.32% | 32.4 |
| post_dispatch | 50,668.0 | 0.896 | 0.896 | 0.776-1.023 | 0.18% | 17.7 |
| root_reaping | 50,668.0 | 0.867 | 0.867 | 0.800-1.042 | 0.17% | 17.1 |
| inbox | 50,668.0 | 0.846 | 0.846 | 0.782-0.996 | 0.17% | 16.7 |
| accept | 7.0 | 0.015 | 0.015 | 0.014-0.019 | 0.00% | 2,077.6 |
| poll_nonblocking | 2.0 | 0.000 | 0.000 | 0.000-0.001 | 0.00% | 221.0 |

`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.

- Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv
- Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency
- The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup
- Listener construction and initial spawn on the main thread precede the worker reset and are excluded
- Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime
- Per-stage medians and median shares need not sum to a median total or 100 percent
- Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile
