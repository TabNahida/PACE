# PACE worker/execution scaling

3 trials per cell, 8 fixed clients, 64-byte messages, 5000 measured + 500 warmup requests/client.

PACE-only loopback echo with one listening port per worker. These results are separate from the Asio/libuv comparison.

| Mode | Workers | Requests/s median | Requests/s min-max | p50 us median | p99 us median |
|---|---:|---:|---:|---:|---:|
| runtime | 1 | 96,939 | 95,431-104,395 | 75.73 | 202.83 |
| caller | 1 | 104,649 | 101,969-106,801 | 75.51 | 88.56 |
| external | 1 | 102,552 | 101,801-104,454 | 76.25 | 90.36 |
| runtime | 2 | 172,882 | 151,009-204,679 | 38.19 | 81.74 |
| caller | 2 | 171,591 | 168,689-206,848 | 38.14 | 79.94 |
| external | 2 | 204,479 | 186,284-207,611 | 37.36 | 73.92 |
| runtime | 4 | 190,415 | 176,590-201,260 | 17.45 | 97.10 |
| caller | 4 | 186,069 | 182,894-203,385 | 18.26 | 167.90 |
| external | 4 | 193,497 | 190,016-209,428 | 17.77 | 86.80 |
| runtime | 8 | 191,350 | 182,766-193,115 | 37.87 | 92.21 |
| caller | 8 | 190,707 | 186,465-204,754 | 40.68 | 74.00 |
| external | 8 | 190,131 | 185,720-190,446 | 28.72 | 104.82 |

Lifecycle times below are medians in milliseconds; raw JSON also preserves every range and trial.

| Mode | Workers | Setup ms | Launch ms | Startup sum ms | Clients/warmup ms | Shutdown ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| runtime | 1 | 0.050 | 0.051 | 0.100 | 39.184 | 0.200 |
| caller | 1 | 0.051 | 0.006 | 0.056 | 39.053 | 0.119 |
| external | 1 | 0.053 | 0.051 | 0.104 | 39.312 | 0.208 |
| runtime | 2 | 0.061 | 0.078 | 0.141 | 20.892 | 0.168 |
| caller | 2 | 0.063 | 0.048 | 0.111 | 20.670 | 0.132 |
| external | 2 | 0.065 | 0.076 | 0.139 | 20.631 | 0.156 |
| runtime | 4 | 0.083 | 0.122 | 0.208 | 18.038 | 0.171 |
| caller | 4 | 0.085 | 0.102 | 0.187 | 18.142 | 0.174 |
| external | 4 | 0.084 | 0.123 | 0.214 | 19.691 | 0.179 |
| runtime | 8 | 0.127 | 0.249 | 0.379 | 21.925 | 0.269 |
| caller | 8 | 0.127 | 0.217 | 0.344 | 22.113 | 0.255 |
| external | 8 | 0.129 | 0.221 | 0.350 | 21.231 | 0.266 |

- Separate PACE-only workload: one port per worker with fixed, evenly balanced persistent clients; no Asio/libuv comparison
- Clients, coordinator and server run on one host and contend for CPU; this is not an isolated server-capacity or linear-scaling estimate
- Changing workers also changes connections per reactor while total clients and requests stay fixed; workers are never inferred from CPU count
- All modes use the same coordinator plus one blocking thread per client; caller mode reduces spawned reactor threads, not the N active reactor threads
- Startup and steady-state windows are separate; warmup and startup are excluded from throughput, but client scheduling and join overhead are included
- TCP_NODELAY, pipeline=1 and loopback echo do not establish HTTP, accept balancing, remote-network or mixed-workload performance
- No CPU affinity or frequency controls are applied; run benchmarks sequentially on an otherwise idle host and inspect trial spread
- Source hashes describe files at runner invocation, not proof that a supplied binary was built from them; retain the compiler command with results
