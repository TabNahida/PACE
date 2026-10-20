# PACE worker/execution scaling

5 trials per cell, 8 fixed clients, 64-byte messages, 10000 measured + 1000 warmup requests/client.

PACE-only loopback echo with one listening port per worker. These results are separate from the Asio/libuv comparison.

| Mode | Workers | Requests/s median | Requests/s min-max | p50 us median | p99 us median |
|---|---:|---:|---:|---:|---:|
| runtime | 1 | 303,599 | 296,602-306,387 | 25.29 | 45.08 |
| caller | 1 | 297,995 | 295,897-302,655 | 25.38 | 48.97 |
| external | 1 | 300,142 | 299,737-305,311 | 25.28 | 48.21 |
| runtime | 2 | 276,037 | 267,295-281,272 | 25.95 | 57.24 |
| caller | 2 | 281,434 | 273,468-290,151 | 25.65 | 55.85 |
| external | 2 | 283,027 | 278,357-284,567 | 25.32 | 55.24 |
| runtime | 4 | 228,556 | 224,309-230,149 | 33.05 | 55.62 |
| caller | 4 | 232,848 | 225,329-234,517 | 32.59 | 54.17 |
| external | 4 | 232,886 | 226,988-235,969 | 32.58 | 54.63 |
| runtime | 8 | 210,373 | 173,269-218,027 | 36.40 | 56.31 |
| caller | 8 | 211,576 | 208,549-218,821 | 36.31 | 55.69 |
| external | 8 | 208,647 | 205,021-218,499 | 36.62 | 56.63 |

Lifecycle times below are medians in milliseconds; raw JSON also preserves every range and trial.

| Mode | Workers | Setup ms | Launch ms | Startup sum ms | Clients/warmup ms | Shutdown ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| runtime | 1 | 0.027 | 0.050 | 0.078 | 26.910 | 0.155 |
| caller | 1 | 0.027 | 0.002 | 0.029 | 27.883 | 0.077 |
| external | 1 | 0.028 | 0.052 | 0.077 | 26.951 | 0.158 |
| runtime | 2 | 0.034 | 0.080 | 0.114 | 29.259 | 0.137 |
| caller | 2 | 0.034 | 0.056 | 0.093 | 28.948 | 0.102 |
| external | 2 | 0.033 | 0.071 | 0.105 | 28.545 | 0.138 |
| runtime | 4 | 0.044 | 0.149 | 0.192 | 35.079 | 0.142 |
| caller | 4 | 0.041 | 0.102 | 0.147 | 34.820 | 0.125 |
| external | 4 | 0.043 | 0.159 | 0.201 | 34.937 | 0.144 |
| runtime | 8 | 0.061 | 0.210 | 0.271 | 37.887 | 0.200 |
| caller | 8 | 0.060 | 0.224 | 0.294 | 38.432 | 0.157 |
| external | 8 | 0.058 | 0.253 | 0.316 | 37.654 | 0.182 |

- Separate PACE-only workload: one port per worker with fixed, evenly balanced persistent clients; no Asio/libuv comparison
- Clients, coordinator and server run on one host and contend for CPU; this is not an isolated server-capacity or linear-scaling estimate
- Changing workers also changes connections per reactor while total clients and requests stay fixed; workers are never inferred from CPU count
- All modes use the same coordinator plus one blocking thread per client; caller mode reduces spawned reactor threads, not the N active reactor threads
- Startup and steady-state windows are separate; warmup and startup are excluded from throughput, but client scheduling and join overhead are included
- TCP_NODELAY, pipeline=1 and loopback echo do not establish HTTP, accept balancing, remote-network or mixed-workload performance
- No CPU affinity or frequency controls are applied; run benchmarks sequentially on an otherwise idle host and inspect trial spread
- Source hashes describe files at runner invocation, not proof that a supplied binary was built from them; retain the compiler command with results
