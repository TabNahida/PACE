# PACE worker/execution scaling

3 trials per cell, 8 fixed clients, 64-byte messages, 5000 measured + 500 warmup requests/client.

PACE-only loopback echo with one listening port per worker. These results are separate from the Asio/libuv comparison.

| Mode | Workers | Requests/s median | Requests/s min-max | p50 us median | p99 us median |
|---|---:|---:|---:|---:|---:|
| runtime | 1 | 378,217 | 375,468-378,702 | 19.68 | 46.09 |
| caller | 1 | 376,670 | 373,151-384,172 | 19.52 | 45.64 |
| external | 1 | 390,240 | 381,000-390,842 | 19.48 | 43.67 |
| runtime | 2 | 451,398 | 436,770-454,445 | 15.24 | 44.43 |
| caller | 2 | 452,224 | 447,005-456,262 | 15.38 | 39.53 |
| external | 2 | 450,599 | 445,497-456,651 | 15.49 | 45.05 |
| runtime | 4 | 432,209 | 429,748-436,697 | 13.01 | 48.48 |
| caller | 4 | 437,920 | 436,523-438,926 | 13.12 | 47.06 |
| external | 4 | 430,778 | 422,623-440,280 | 12.98 | 55.45 |
| runtime | 8 | 400,747 | 399,156-405,022 | 16.17 | 94.48 |
| caller | 8 | 401,783 | 400,048-405,144 | 16.18 | 97.39 |
| external | 8 | 401,921 | 401,001-402,374 | 16.12 | 92.05 |

Lifecycle times below are medians in milliseconds; raw JSON also preserves every range and trial.

| Mode | Workers | Setup ms | Launch ms | Startup sum ms | Clients/warmup ms | Shutdown ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| runtime | 1 | 0.048 | 0.107 | 0.155 | 11.361 | 0.228 |
| caller | 1 | 0.050 | 0.006 | 0.056 | 10.813 | 0.146 |
| external | 1 | 0.050 | 0.107 | 0.161 | 11.500 | 0.221 |
| runtime | 2 | 0.063 | 0.131 | 0.187 | 9.705 | 0.296 |
| caller | 2 | 0.058 | 0.099 | 0.154 | 9.714 | 0.218 |
| external | 2 | 0.060 | 0.136 | 0.200 | 9.181 | 0.302 |
| runtime | 4 | 0.067 | 0.208 | 0.275 | 9.971 | 0.619 |
| caller | 4 | 0.068 | 0.161 | 0.228 | 10.682 | 0.533 |
| external | 4 | 0.071 | 0.195 | 0.266 | 10.257 | 0.608 |
| runtime | 8 | 0.094 | 0.272 | 0.366 | 10.631 | 0.908 |
| caller | 8 | 0.100 | 0.278 | 0.377 | 10.370 | 0.855 |
| external | 8 | 0.094 | 0.286 | 0.382 | 10.252 | 0.912 |

- Separate PACE-only workload: one port per worker with fixed, evenly balanced persistent clients; no Asio/libuv comparison
- Clients, coordinator and server run on one host and contend for CPU; this is not an isolated server-capacity or linear-scaling estimate
- Changing workers also changes connections per reactor while total clients and requests stay fixed; workers are never inferred from CPU count
- All modes use the same coordinator plus one blocking thread per client; caller mode reduces spawned reactor threads, not the N active reactor threads
- Startup and steady-state windows are separate; warmup and startup are excluded from throughput, but client scheduling and join overhead are included
- TCP_NODELAY, pipeline=1 and loopback echo do not establish HTTP, accept balancing, remote-network or mixed-workload performance
- No CPU affinity or frequency controls are applied; run benchmarks sequentially on an otherwise idle host and inspect trial spread
- Source hashes describe files at runner invocation, not proof that a supplied binary was built from them; retain the compiler command with results
