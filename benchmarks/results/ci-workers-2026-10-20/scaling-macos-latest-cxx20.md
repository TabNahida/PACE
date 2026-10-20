# PACE worker/execution scaling

3 trials per cell, 8 fixed clients, 64-byte messages, 5000 measured + 500 warmup requests/client.

PACE-only loopback echo with one listening port per worker. These results are separate from the Asio/libuv comparison.

| Mode | Workers | Requests/s median | Requests/s min-max | p50 us median | p99 us median |
|---|---:|---:|---:|---:|---:|
| runtime | 1 | 66,948 | 50,405-73,551 | 107.00 | 277.12 |
| caller | 1 | 61,658 | 56,803-69,205 | 125.42 | 484.67 |
| external | 1 | 61,568 | 59,731-71,808 | 132.71 | 301.75 |
| runtime | 2 | 60,976 | 52,503-88,254 | 113.62 | 402.88 |
| caller | 2 | 66,072 | 63,766-98,770 | 105.92 | 271.67 |
| external | 2 | 66,941 | 49,408-74,624 | 83.79 | 611.12 |
| runtime | 4 | 70,215 | 57,699-84,850 | 83.38 | 306.92 |
| caller | 4 | 73,670 | 65,749-85,684 | 78.21 | 340.67 |
| external | 4 | 67,411 | 52,829-69,469 | 85.29 | 484.04 |
| runtime | 8 | 52,936 | 51,968-61,885 | 112.71 | 422.75 |
| caller | 8 | 52,635 | 51,277-53,163 | 144.50 | 364.50 |
| external | 8 | 51,144 | 48,527-78,408 | 166.25 | 330.88 |

Lifecycle times below are medians in milliseconds; raw JSON also preserves every range and trial.

| Mode | Workers | Setup ms | Launch ms | Startup sum ms | Clients/warmup ms | Shutdown ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| runtime | 1 | 0.121 | 0.056 | 0.177 | 68.278 | 0.182 |
| caller | 1 | 0.056 | 0.009 | 0.065 | 61.712 | 0.177 |
| external | 1 | 0.095 | 0.040 | 0.138 | 60.592 | 0.180 |
| runtime | 2 | 0.100 | 0.030 | 0.131 | 65.235 | 0.251 |
| caller | 2 | 0.102 | 0.049 | 0.139 | 58.857 | 0.181 |
| external | 2 | 0.072 | 0.038 | 0.112 | 56.589 | 0.219 |
| runtime | 4 | 0.199 | 0.134 | 0.334 | 68.936 | 0.190 |
| caller | 4 | 0.092 | 0.069 | 0.161 | 78.290 | 0.227 |
| external | 4 | 0.096 | 0.087 | 0.195 | 64.976 | 0.212 |
| runtime | 8 | 0.238 | 0.190 | 0.455 | 86.510 | 0.315 |
| caller | 8 | 0.299 | 0.164 | 0.463 | 78.359 | 0.285 |
| external | 8 | 0.154 | 0.116 | 0.238 | 76.760 | 0.292 |

- Separate PACE-only workload: one port per worker with fixed, evenly balanced persistent clients; no Asio/libuv comparison
- Clients, coordinator and server run on one host and contend for CPU; this is not an isolated server-capacity or linear-scaling estimate
- Changing workers also changes connections per reactor while total clients and requests stay fixed; workers are never inferred from CPU count
- All modes use the same coordinator plus one blocking thread per client; caller mode reduces spawned reactor threads, not the N active reactor threads
- Startup and steady-state windows are separate; warmup and startup are excluded from throughput, but client scheduling and join overhead are included
- TCP_NODELAY, pipeline=1 and loopback echo do not establish HTTP, accept balancing, remote-network or mixed-workload performance
- No CPU affinity or frequency controls are applied; run benchmarks sequentially on an otherwise idle host and inspect trial spread
- Source hashes describe files at runner invocation, not proof that a supplied binary was built from them; retain the compiler command with results
