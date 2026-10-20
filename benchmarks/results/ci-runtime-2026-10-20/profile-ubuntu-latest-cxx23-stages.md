# PACE stage timing diagnostic

**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**

3 trials. Values below are medians across trials; ranges show exclusive-time spread.

| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |
|---|---:|---:|---:|---:|---:|---:|
| send | 44,000.0 | 539.043 | 539.043 | 537.878-543.685 | 70.52% | 12,251.0 |
| recv | 87,988.0 | 120.665 | 120.665 | 120.298-121.324 | 15.77% | 1,371.4 |
| poller_update | 43,986.0 | 33.719 | 33.719 | 33.712-34.005 | 4.42% | 766.7 |
| coroutine_execution | 16,358.0 | 729.810 | 30.638 | 29.754-31.132 | 3.99% | 44,614.9 |
| poll_blocking | 16,355.0 | 23.023 | 23.023 | 22.852-25.566 | 3.02% | 1,426.7 |
| io_arm | 43,986.0 | 40.006 | 6.293 | 5.927-6.751 | 0.82% | 909.8 |
| reactor_run | 1.0 | 765.124 | 5.093 | 4.838-5.266 | 0.67% | 765,124,171.0 |
| event_dispatch | 16,356.0 | 1.653 | 1.653 | 1.362-1.711 | 0.22% | 101.1 |
| timers | 16,356.0 | 1.347 | 1.347 | 1.312-1.366 | 0.18% | 82.3 |
| root_reaping | 16,358.0 | 0.745 | 0.745 | 0.722-0.784 | 0.10% | 45.6 |
| post_dispatch | 16,358.0 | 0.709 | 0.709 | 0.670-0.733 | 0.09% | 43.4 |
| inbox | 16,358.0 | 0.586 | 0.586 | 0.569-0.594 | 0.08% | 35.5 |
| accept | 6.0 | 0.042 | 0.042 | 0.032-0.061 | 0.01% | 7,070.0 |
| poll_nonblocking | 1.0 | 0.001 | 0.001 | 0.001-0.002 | 0.00% | 762.0 |

`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.

- Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv
- Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency
- The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup
- Listener construction and initial spawn on the main thread precede the worker reset and are excluded
- Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime
- Per-stage medians and median shares need not sum to a median total or 100 percent
- Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile
