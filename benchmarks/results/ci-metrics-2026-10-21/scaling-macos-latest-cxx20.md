# PACE worker/execution scaling

3 trials per cell, 8 fixed clients, 64-byte messages, 5000 measured + 500 warmup requests/client.

PACE-only loopback echo with one listening port per worker. These results are separate from the Asio/libuv comparison.

| Mode | Workers | Requests/s median | Requests/s min-max | p50 us median | p99 us median |
|---|---:|---:|---:|---:|---:|
| runtime | 1 | 63,590 | 53,450-105,349 | 116.08 | 311.96 |
| caller | 1 | 72,575 | 55,179-82,069 | 71.21 | 463.08 |
| external | 1 | 65,618 | 56,149-70,693 | 102.71 | 342.12 |
| runtime | 2 | 60,035 | 58,499-65,061 | 115.88 | 474.58 |
| caller | 2 | 58,844 | 53,342-63,845 | 119.75 | 455.21 |
| external | 2 | 57,092 | 55,739-71,084 | 98.42 | 393.96 |
| runtime | 4 | 53,725 | 49,353-65,215 | 130.54 | 447.29 |
| caller | 4 | 55,760 | 54,321-72,566 | 103.71 | 584.12 |
| external | 4 | 60,126 | 56,339-77,222 | 93.67 | 562.50 |
| runtime | 8 | 54,744 | 50,435-62,195 | 116.88 | 519.33 |
| caller | 8 | 55,773 | 50,891-66,943 | 126.38 | 593.46 |
| external | 8 | 64,887 | 55,120-65,623 | 87.67 | 491.08 |

Lifecycle times below are medians in milliseconds; raw JSON also preserves every range and trial.

| Mode | Workers | Setup ms | Launch ms | Startup sum ms | Clients/warmup ms | Shutdown ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| runtime | 1 | 0.064 | 0.030 | 0.099 | 74.562 | 0.219 |
| caller | 1 | 0.070 | 0.011 | 0.082 | 63.436 | 0.122 |
| external | 1 | 0.065 | 0.026 | 0.092 | 36.554 | 0.153 |
| runtime | 2 | 0.075 | 0.042 | 0.114 | 73.584 | 0.220 |
| caller | 2 | 0.073 | 0.046 | 0.113 | 64.478 | 0.287 |
| external | 2 | 0.095 | 0.050 | 0.126 | 61.330 | 0.207 |
| runtime | 4 | 0.096 | 0.080 | 0.176 | 83.693 | 0.265 |
| caller | 4 | 0.091 | 0.043 | 0.134 | 81.199 | 0.172 |
| external | 4 | 0.104 | 0.113 | 0.234 | 70.480 | 0.248 |
| runtime | 8 | 0.170 | 0.163 | 0.342 | 81.221 | 0.311 |
| caller | 8 | 0.145 | 0.171 | 0.304 | 91.863 | 0.287 |
| external | 8 | 0.143 | 0.092 | 0.234 | 89.383 | 0.279 |

- Separate PACE-only workload: one port per worker with fixed, evenly balanced persistent clients; no Asio/libuv comparison
- Clients, coordinator and server run on one host and contend for CPU; this is not an isolated server-capacity or linear-scaling estimate
- Changing workers also changes connections per reactor while total clients and requests stay fixed; workers are never inferred from CPU count
- All modes use the same coordinator plus one blocking thread per client; caller mode reduces spawned reactor threads, not the N active reactor threads
- Startup and steady-state windows are separate; warmup and startup are excluded from throughput, but client scheduling and join overhead are included
- TCP_NODELAY, pipeline=1 and loopback echo do not establish HTTP, accept balancing, remote-network or mixed-workload performance
- No CPU affinity or frequency controls are applied; run benchmarks sequentially on an otherwise idle host and inspect trial spread
- Source hashes describe files at runner invocation, not proof that a supplied binary was built from them; retain the compiler command with results
