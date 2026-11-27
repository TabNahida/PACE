# PACE worker/execution scaling

5 trials per cell, 8 fixed clients, 64-byte messages, 2000 measured + 200 warmup requests/client.

PACE-only loopback echo with one listening port per worker. These results are separate from the Asio/libuv comparison.

| Mode | Workers | Requests/s median | Requests/s min-max | p50 us median | p99 us median |
|---|---:|---:|---:|---:|---:|
| runtime | 1 | 227,144 | 223,560-233,246 | 31.62 | 87.79 |
| caller | 1 | 226,760 | 221,750-227,788 | 31.67 | 88.79 |
| external | 1 | 226,054 | 206,160-228,426 | 31.79 | 87.62 |
| runtime | 2 | 259,619 | 239,514-268,479 | 28.96 | 58.96 |
| caller | 2 | 263,376 | 261,519-268,778 | 28.42 | 58.50 |
| external | 2 | 262,697 | 261,984-264,605 | 28.46 | 58.79 |
| runtime | 4 | 237,828 | 237,105-247,060 | 31.88 | 60.33 |
| caller | 4 | 237,731 | 236,803-246,980 | 31.83 | 60.17 |
| external | 4 | 237,711 | 234,754-239,036 | 31.83 | 61.46 |
| runtime | 8 | 212,234 | 209,954-214,371 | 36.42 | 64.17 |
| caller | 8 | 211,696 | 210,979-212,989 | 36.33 | 64.42 |
| external | 8 | 215,138 | 213,324-217,437 | 35.88 | 63.00 |

Lifecycle times below are medians in milliseconds; raw JSON also preserves every range and trial.

| Mode | Workers | Setup ms | Launch ms | Startup sum ms | Clients/warmup ms | Shutdown ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| runtime | 1 | 0.022 | 0.015 | 0.037 | 7.283 | 0.130 |
| caller | 1 | 0.021 | 0.003 | 0.024 | 7.576 | 0.130 |
| external | 1 | 0.022 | 0.014 | 0.038 | 7.214 | 0.134 |
| runtime | 2 | 0.028 | 0.013 | 0.042 | 6.359 | 0.135 |
| caller | 2 | 0.029 | 0.011 | 0.045 | 6.219 | 0.139 |
| external | 2 | 0.028 | 0.014 | 0.041 | 6.221 | 0.147 |
| runtime | 4 | 0.036 | 0.022 | 0.058 | 7.059 | 0.162 |
| caller | 4 | 0.040 | 0.021 | 0.061 | 6.783 | 0.143 |
| external | 4 | 0.037 | 0.022 | 0.065 | 7.005 | 0.160 |
| runtime | 8 | 0.058 | 0.038 | 0.097 | 7.704 | 0.175 |
| caller | 8 | 0.058 | 0.036 | 0.103 | 7.676 | 0.168 |
| external | 8 | 0.060 | 0.037 | 0.101 | 7.772 | 0.174 |

- Separate PACE-only workload: one port per worker with fixed, evenly balanced persistent clients; no Asio/libuv comparison
- Clients, coordinator and server run on one host and contend for CPU; this is not an isolated server-capacity or linear-scaling estimate
- Changing workers also changes connections per reactor while total clients and requests stay fixed; workers are never inferred from CPU count
- All modes use the same coordinator plus one blocking thread per client; caller mode reduces spawned reactor threads, not the N active reactor threads
- Startup and steady-state windows are separate; warmup and startup are excluded from throughput, but client scheduling and join overhead are included
- TCP_NODELAY, pipeline=1 and loopback echo do not establish HTTP, accept balancing, remote-network or mixed-workload performance
- No CPU affinity or frequency controls are applied; run benchmarks sequentially on an otherwise idle host and inspect trial spread
- Source hashes describe files at runner invocation, not proof that a supplied binary was built from them; retain the compiler command with results
