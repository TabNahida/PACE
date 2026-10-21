# PACE worker/execution scaling

3 trials per cell, 8 fixed clients, 64-byte messages, 5000 measured + 500 warmup requests/client.

PACE-only loopback echo with one listening port per worker. These results are separate from the Asio/libuv comparison.

| Mode | Workers | Requests/s median | Requests/s min-max | p50 us median | p99 us median |
|---|---:|---:|---:|---:|---:|
| runtime | 1 | 119,409 | 110,378-120,818 | 66.06 | 80.39 |
| caller | 1 | 114,486 | 113,908-118,554 | 69.69 | 84.58 |
| external | 1 | 118,807 | 110,332-121,042 | 66.05 | 82.24 |
| runtime | 2 | 256,335 | 241,594-294,559 | 25.01 | 64.24 |
| caller | 2 | 242,536 | 189,108-249,018 | 27.05 | 148.59 |
| external | 2 | 238,865 | 197,866-263,993 | 29.33 | 68.51 |
| runtime | 4 | 257,822 | 216,266-449,570 | 8.65 | 70.95 |
| caller | 4 | 216,615 | 204,748-237,044 | 26.32 | 84.04 |
| external | 4 | 226,253 | 205,890-246,805 | 16.18 | 86.91 |
| runtime | 8 | 309,272 | 306,033-364,460 | 15.58 | 51.98 |
| caller | 8 | 363,398 | 362,587-384,496 | 20.52 | 35.63 |
| external | 8 | 369,686 | 340,519-399,443 | 18.89 | 48.78 |

Lifecycle times below are medians in milliseconds; raw JSON also preserves every range and trial.

| Mode | Workers | Setup ms | Launch ms | Startup sum ms | Clients/warmup ms | Shutdown ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| runtime | 1 | 0.053 | 0.069 | 0.123 | 34.732 | 0.165 |
| caller | 1 | 0.046 | 0.006 | 0.051 | 36.609 | 0.123 |
| external | 1 | 0.057 | 0.062 | 0.119 | 33.642 | 0.157 |
| runtime | 2 | 0.067 | 0.106 | 0.188 | 15.667 | 0.174 |
| caller | 2 | 0.062 | 0.058 | 0.123 | 15.835 | 0.147 |
| external | 2 | 0.062 | 0.094 | 0.156 | 17.868 | 0.167 |
| runtime | 4 | 0.092 | 0.169 | 0.271 | 9.719 | 0.218 |
| caller | 4 | 0.081 | 0.128 | 0.209 | 13.772 | 0.210 |
| external | 4 | 0.082 | 0.155 | 0.242 | 13.713 | 0.198 |
| runtime | 8 | 0.128 | 0.250 | 0.371 | 12.346 | 0.267 |
| caller | 8 | 0.117 | 0.254 | 0.371 | 13.016 | 0.277 |
| external | 8 | 0.110 | 0.273 | 0.383 | 12.393 | 0.289 |

- Separate PACE-only workload: one port per worker with fixed, evenly balanced persistent clients; no Asio/libuv comparison
- Clients, coordinator and server run on one host and contend for CPU; this is not an isolated server-capacity or linear-scaling estimate
- Changing workers also changes connections per reactor while total clients and requests stay fixed; workers are never inferred from CPU count
- All modes use the same coordinator plus one blocking thread per client; caller mode reduces spawned reactor threads, not the N active reactor threads
- Startup and steady-state windows are separate; warmup and startup are excluded from throughput, but client scheduling and join overhead are included
- TCP_NODELAY, pipeline=1 and loopback echo do not establish HTTP, accept balancing, remote-network or mixed-workload performance
- No CPU affinity or frequency controls are applied; run benchmarks sequentially on an otherwise idle host and inspect trial spread
- Source hashes describe files at runner invocation, not proof that a supplied binary was built from them; retain the compiler command with results
