# PACE stage timing diagnostic

**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**

3 trials. Values below are medians across trials; ranges show exclusive-time spread.

| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |
|---|---:|---:|---:|---:|---:|---:|
| send | 352,000.0 | 835.619 | 835.619 | 824.730-837.850 | 67.71% | 2,373.9 |
| recv | 704,004.0 | 181.271 | 181.271 | 179.171-181.347 | 14.65% | 257.5 |
| poller_update | 351,977.0 | 75.456 | 75.456 | 73.285-75.955 | 6.12% | 214.4 |
| coroutine_execution | 13,113.0 | 1192.081 | 43.416 | 43.157-43.952 | 3.52% | 90,908.3 |
| accept | 36.0 | 39.259 | 39.259 | 33.649-49.457 | 3.17% | 1,061,048.5 |
| poll_blocking | 13,109.0 | 37.242 | 37.242 | 36.782-37.890 | 3.01% | 2,842.5 |
| io_arm | 351,977.0 | 95.333 | 19.672 | 19.378-20.110 | 1.59% | 270.9 |
| event_dispatch | 13,112.0 | 2.126 | 2.126 | 2.041-2.186 | 0.17% | 162.2 |
| reactor_run | 1.0 | 1233.834 | 1.612 | 1.585-1.634 | 0.13% | 1,233,834,232.0 |
| timers | 13,112.0 | 0.416 | 0.416 | 0.409-0.422 | 0.03% | 31.8 |
| root_reaping | 13,113.0 | 0.398 | 0.398 | 0.388-0.435 | 0.03% | 30.4 |
| inbox | 13,113.0 | 0.239 | 0.239 | 0.237-0.246 | 0.02% | 18.2 |
| post_dispatch | 13,113.0 | 0.211 | 0.211 | 0.204-0.535 | 0.02% | 16.1 |
| poll_nonblocking | 3.0 | 0.004 | 0.004 | 0.003-0.008 | 0.00% | 1,472.7 |

`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.

- Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv
- Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency
- The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup
- Listener construction and initial spawn on the main thread precede the worker reset and are excluded
- Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime
- Per-stage medians and median shares need not sum to a median total or 100 percent
- Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile
