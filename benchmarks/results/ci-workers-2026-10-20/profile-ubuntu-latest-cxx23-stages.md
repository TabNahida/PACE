# PACE stage timing diagnostic

**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**

3 trials. Values below are medians across trials; ranges show exclusive-time spread.

| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |
|---|---:|---:|---:|---:|---:|---:|
| send | 44,000.0 | 463.590 | 463.590 | 453.997-483.816 | 68.18% | 10,536.1 |
| recv | 87,969.0 | 114.973 | 114.973 | 113.329-117.214 | 16.91% | 1,307.1 |
| poller_update | 43,967.0 | 32.850 | 32.850 | 32.809-32.876 | 4.83% | 747.2 |
| coroutine_execution | 18,346.0 | 645.040 | 27.682 | 26.648-28.777 | 4.07% | 35,159.7 |
| poll_blocking | 18,344.0 | 22.864 | 22.864 | 21.775-24.399 | 3.43% | 1,259.5 |
| io_arm | 43,967.0 | 38.753 | 5.904 | 5.844-6.769 | 0.87% | 881.5 |
| reactor_run | 1.0 | 679.951 | 5.180 | 4.901-5.284 | 0.78% | 679,951,320.0 |
| timers | 18,345.0 | 1.629 | 1.629 | 1.521-1.644 | 0.23% | 88.8 |
| event_dispatch | 18,345.0 | 1.517 | 1.517 | 1.488-1.695 | 0.22% | 82.7 |
| root_reaping | 18,346.0 | 0.748 | 0.748 | 0.716-0.768 | 0.11% | 40.8 |
| post_dispatch | 18,346.0 | 0.667 | 0.667 | 0.566-0.725 | 0.10% | 36.4 |
| inbox | 18,346.0 | 0.667 | 0.667 | 0.618-0.676 | 0.10% | 35.8 |
| accept | 6.0 | 0.040 | 0.040 | 0.039-0.044 | 0.01% | 6,674.2 |
| poll_nonblocking | 1.0 | 0.001 | 0.001 | 0.001-0.001 | 0.00% | 591.0 |

`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.

- Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv
- Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency
- The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup
- Listener construction and initial spawn on the main thread precede the worker reset and are excluded
- Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime
- Per-stage medians and median shares need not sum to a median total or 100 percent
- Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile
