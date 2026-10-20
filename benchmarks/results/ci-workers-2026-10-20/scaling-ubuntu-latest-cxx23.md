# PACE worker/execution scaling

3 trials per cell, 8 fixed clients, 64-byte messages, 5000 measured + 500 warmup requests/client.

PACE-only loopback echo with one listening port per worker. These results are separate from the Asio/libuv comparison.

| Mode | Workers | Requests/s median | Requests/s min-max | p50 us median | p99 us median |
|---|---:|---:|---:|---:|---:|
| runtime | 1 | 67,342 | 67,032-67,514 | 116.62 | 142.62 |
| caller | 1 | 67,102 | 66,975-67,386 | 116.98 | 144.56 |
| external | 1 | 66,992 | 66,842-67,634 | 116.93 | 143.38 |
| runtime | 2 | 120,040 | 112,090-132,328 | 54.14 | 236.69 |
| caller | 2 | 113,836 | 108,965-126,495 | 54.97 | 275.72 |
| external | 2 | 126,932 | 126,330-127,899 | 55.37 | 139.06 |
| runtime | 4 | 119,421 | 108,852-137,755 | 46.21 | 177.22 |
| caller | 4 | 115,897 | 112,387-125,048 | 48.10 | 180.70 |
| external | 4 | 117,192 | 115,913-129,321 | 50.10 | 173.78 |
| runtime | 8 | 129,018 | 128,867-131,563 | 56.92 | 113.41 |
| caller | 8 | 121,919 | 116,966-135,197 | 45.94 | 146.60 |
| external | 8 | 131,540 | 130,806-131,802 | 57.05 | 115.33 |

Lifecycle times below are medians in milliseconds; raw JSON also preserves every range and trial.

| Mode | Workers | Setup ms | Launch ms | Startup sum ms | Clients/warmup ms | Shutdown ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| runtime | 1 | 0.054 | 0.075 | 0.129 | 59.249 | 0.218 |
| caller | 1 | 0.070 | 0.006 | 0.076 | 59.694 | 0.100 |
| external | 1 | 0.051 | 0.067 | 0.126 | 58.555 | 0.213 |
| runtime | 2 | 0.066 | 0.096 | 0.163 | 36.490 | 0.184 |
| caller | 2 | 0.065 | 0.071 | 0.134 | 33.805 | 0.162 |
| external | 2 | 0.066 | 0.100 | 0.166 | 29.774 | 0.172 |
| runtime | 4 | 0.096 | 0.225 | 0.356 | 32.193 | 0.225 |
| caller | 4 | 0.094 | 0.153 | 0.248 | 33.345 | 0.209 |
| external | 4 | 0.098 | 0.212 | 0.315 | 28.669 | 0.216 |
| runtime | 8 | 0.157 | 0.397 | 0.551 | 32.481 | 0.334 |
| caller | 8 | 0.158 | 0.336 | 0.507 | 30.833 | 0.309 |
| external | 8 | 0.155 | 0.357 | 0.511 | 31.011 | 0.305 |

- Separate PACE-only workload: one port per worker with fixed, evenly balanced persistent clients; no Asio/libuv comparison
- Clients, coordinator and server run on one host and contend for CPU; this is not an isolated server-capacity or linear-scaling estimate
- Changing workers also changes connections per reactor while total clients and requests stay fixed; workers are never inferred from CPU count
- All modes use the same coordinator plus one blocking thread per client; caller mode reduces spawned reactor threads, not the N active reactor threads
- Startup and steady-state windows are separate; warmup and startup are excluded from throughput, but client scheduling and join overhead are included
- TCP_NODELAY, pipeline=1 and loopback echo do not establish HTTP, accept balancing, remote-network or mixed-workload performance
- No CPU affinity or frequency controls are applied; run benchmarks sequentially on an otherwise idle host and inspect trial spread
- Source hashes describe files at runner invocation, not proof that a supplied binary was built from them; retain the compiler command with results
