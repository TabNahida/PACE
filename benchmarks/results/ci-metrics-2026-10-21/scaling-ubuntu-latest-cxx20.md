# PACE worker/execution scaling

3 trials per cell, 8 fixed clients, 64-byte messages, 5000 measured + 500 warmup requests/client.

PACE-only loopback echo with one listening port per worker. These results are separate from the Asio/libuv comparison.

| Mode | Workers | Requests/s median | Requests/s min-max | p50 us median | p99 us median |
|---|---:|---:|---:|---:|---:|
| runtime | 1 | 69,920 | 69,730-70,414 | 111.95 | 133.92 |
| caller | 1 | 70,026 | 68,890-70,090 | 111.97 | 134.84 |
| external | 1 | 69,619 | 68,666-69,731 | 112.15 | 134.62 |
| runtime | 2 | 123,296 | 121,280-131,092 | 58.01 | 127.10 |
| caller | 2 | 126,005 | 122,071-129,225 | 53.82 | 204.46 |
| external | 2 | 123,684 | 116,081-134,178 | 55.76 | 120.96 |
| runtime | 4 | 126,536 | 123,897-140,458 | 26.50 | 306.58 |
| caller | 4 | 135,354 | 129,496-156,210 | 25.39 | 267.86 |
| external | 4 | 129,375 | 121,159-137,771 | 27.47 | 415.07 |
| runtime | 8 | 128,403 | 127,464-131,664 | 59.50 | 126.00 |
| caller | 8 | 131,398 | 130,656-131,967 | 59.96 | 103.06 |
| external | 8 | 128,465 | 128,347-131,996 | 59.88 | 102.86 |

Lifecycle times below are medians in milliseconds; raw JSON also preserves every range and trial.

| Mode | Workers | Setup ms | Launch ms | Startup sum ms | Clients/warmup ms | Shutdown ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| runtime | 1 | 0.061 | 0.073 | 0.141 | 57.322 | 0.253 |
| caller | 1 | 0.068 | 0.007 | 0.076 | 58.605 | 0.128 |
| external | 1 | 0.064 | 0.072 | 0.136 | 59.825 | 0.255 |
| runtime | 2 | 0.080 | 0.121 | 0.201 | 32.648 | 0.234 |
| caller | 2 | 0.104 | 0.069 | 0.175 | 34.503 | 0.174 |
| external | 2 | 0.079 | 0.106 | 0.186 | 30.398 | 0.196 |
| runtime | 4 | 0.120 | 0.177 | 0.298 | 26.291 | 0.234 |
| caller | 4 | 0.115 | 0.144 | 0.260 | 26.044 | 0.223 |
| external | 4 | 0.114 | 0.195 | 0.312 | 31.275 | 0.237 |
| runtime | 8 | 0.193 | 0.364 | 0.558 | 32.621 | 0.315 |
| caller | 8 | 0.195 | 0.289 | 0.496 | 30.781 | 0.333 |
| external | 8 | 0.182 | 0.380 | 0.572 | 30.393 | 0.333 |

- Separate PACE-only workload: one port per worker with fixed, evenly balanced persistent clients; no Asio/libuv comparison
- Clients, coordinator and server run on one host and contend for CPU; this is not an isolated server-capacity or linear-scaling estimate
- Changing workers also changes connections per reactor while total clients and requests stay fixed; workers are never inferred from CPU count
- All modes use the same coordinator plus one blocking thread per client; caller mode reduces spawned reactor threads, not the N active reactor threads
- Startup and steady-state windows are separate; warmup and startup are excluded from throughput, but client scheduling and join overhead are included
- TCP_NODELAY, pipeline=1 and loopback echo do not establish HTTP, accept balancing, remote-network or mixed-workload performance
- No CPU affinity or frequency controls are applied; run benchmarks sequentially on an otherwise idle host and inspect trial spread
- Source hashes describe files at runner invocation, not proof that a supplied binary was built from them; retain the compiler command with results
