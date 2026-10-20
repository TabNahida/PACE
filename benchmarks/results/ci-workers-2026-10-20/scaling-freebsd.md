# PACE worker/execution scaling

3 trials per cell, 8 fixed clients, 64-byte messages, 5000 measured + 500 warmup requests/client.

PACE-only loopback echo with one listening port per worker. These results are separate from the Asio/libuv comparison.

| Mode | Workers | Requests/s median | Requests/s min-max | p50 us median | p99 us median |
|---|---:|---:|---:|---:|---:|
| runtime | 1 | 259,126 | 253,413-263,935 | 28.24 | 71.73 |
| caller | 1 | 259,016 | 256,849-260,979 | 28.07 | 72.64 |
| external | 1 | 259,547 | 254,842-262,949 | 28.13 | 73.01 |
| runtime | 2 | 273,775 | 272,722-275,574 | 25.07 | 69.24 |
| caller | 2 | 274,296 | 270,660-274,625 | 25.11 | 68.75 |
| external | 2 | 273,068 | 265,808-274,773 | 24.99 | 67.99 |
| runtime | 4 | 268,238 | 267,250-269,363 | 20.96 | 66.26 |
| caller | 4 | 265,059 | 261,172-267,365 | 20.49 | 61.85 |
| external | 4 | 266,653 | 263,878-271,172 | 23.05 | 65.81 |
| runtime | 8 | 257,059 | 251,947-259,829 | 25.22 | 69.29 |
| caller | 8 | 259,189 | 256,220-260,554 | 25.46 | 69.47 |
| external | 8 | 252,738 | 224,252-253,259 | 25.08 | 69.41 |

Lifecycle times below are medians in milliseconds; raw JSON also preserves every range and trial.

| Mode | Workers | Setup ms | Launch ms | Startup sum ms | Clients/warmup ms | Shutdown ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| runtime | 1 | 0.055 | 0.160 | 0.214 | 16.850 | 0.271 |
| caller | 1 | 0.058 | 0.008 | 0.065 | 16.685 | 0.186 |
| external | 1 | 0.057 | 0.161 | 0.218 | 17.027 | 0.246 |
| runtime | 2 | 0.069 | 0.205 | 0.271 | 16.768 | 0.394 |
| caller | 2 | 0.085 | 0.103 | 0.187 | 18.644 | 0.249 |
| external | 2 | 0.065 | 0.171 | 0.236 | 15.651 | 0.416 |
| runtime | 4 | 0.079 | 0.267 | 0.346 | 17.156 | 0.753 |
| caller | 4 | 0.080 | 0.299 | 0.387 | 18.204 | 0.678 |
| external | 4 | 0.077 | 0.240 | 0.318 | 16.248 | 0.789 |
| runtime | 8 | 0.106 | 0.458 | 0.564 | 18.451 | 1.083 |
| caller | 8 | 0.106 | 0.480 | 0.583 | 17.986 | 1.106 |
| external | 8 | 0.106 | 0.418 | 0.524 | 17.226 | 1.165 |

- Separate PACE-only workload: one port per worker with fixed, evenly balanced persistent clients; no Asio/libuv comparison
- Clients, coordinator and server run on one host and contend for CPU; this is not an isolated server-capacity or linear-scaling estimate
- Changing workers also changes connections per reactor while total clients and requests stay fixed; workers are never inferred from CPU count
- All modes use the same coordinator plus one blocking thread per client; caller mode reduces spawned reactor threads, not the N active reactor threads
- Startup and steady-state windows are separate; warmup and startup are excluded from throughput, but client scheduling and join overhead are included
- TCP_NODELAY, pipeline=1 and loopback echo do not establish HTTP, accept balancing, remote-network or mixed-workload performance
- No CPU affinity or frequency controls are applied; run benchmarks sequentially on an otherwise idle host and inspect trial spread
- Source hashes describe files at runner invocation, not proof that a supplied binary was built from them; retain the compiler command with results
