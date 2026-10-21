# PACE worker/execution scaling

3 trials per cell, 8 fixed clients, 64-byte messages, 5000 measured + 500 warmup requests/client.

PACE-only loopback echo with one listening port per worker. These results are separate from the Asio/libuv comparison.

| Mode | Workers | Requests/s median | Requests/s min-max | p50 us median | p99 us median |
|---|---:|---:|---:|---:|---:|
| runtime | 1 | 36,924 | 36,833-37,068 | 211.30 | 443.60 |
| caller | 1 | 37,066 | 36,687-37,283 | 211.50 | 430.10 |
| external | 1 | 36,990 | 36,798-37,289 | 211.50 | 430.60 |
| runtime | 2 | 52,910 | 52,906-53,074 | 116.60 | 589.80 |
| caller | 2 | 52,774 | 52,660-52,798 | 117.60 | 601.10 |
| external | 2 | 53,236 | 52,976-53,500 | 115.30 | 592.70 |
| runtime | 4 | 75,729 | 73,045-80,727 | 56.40 | 778.10 |
| caller | 4 | 75,236 | 75,003-80,164 | 56.30 | 814.80 |
| external | 4 | 74,879 | 73,452-79,271 | 55.20 | 725.90 |
| runtime | 8 | 80,355 | 79,223-86,413 | 38.30 | 169.20 |
| caller | 8 | 82,014 | 80,809-83,351 | 38.70 | 187.00 |
| external | 8 | 76,134 | 72,594-77,822 | 40.10 | 174.10 |

Lifecycle times below are medians in milliseconds; raw JSON also preserves every range and trial.

| Mode | Workers | Setup ms | Launch ms | Startup sum ms | Clients/warmup ms | Shutdown ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| runtime | 1 | 0.849 | 0.489 | 1.320 | 107.961 | 0.228 |
| caller | 1 | 0.775 | 0.004 | 0.779 | 108.995 | 0.156 |
| external | 1 | 0.800 | 0.495 | 1.363 | 107.628 | 0.231 |
| runtime | 2 | 1.011 | 0.548 | 1.569 | 54.974 | 0.357 |
| caller | 2 | 1.033 | 0.494 | 1.532 | 60.092 | 0.257 |
| external | 2 | 0.994 | 0.568 | 1.593 | 45.444 | 0.328 |
| runtime | 4 | 1.439 | 0.729 | 2.168 | 53.463 | 0.517 |
| caller | 4 | 1.350 | 0.681 | 2.031 | 55.765 | 0.437 |
| external | 4 | 1.393 | 0.722 | 2.110 | 53.467 | 0.479 |
| runtime | 8 | 2.134 | 1.042 | 3.169 | 56.520 | 0.708 |
| caller | 8 | 2.421 | 0.974 | 3.571 | 57.372 | 0.767 |
| external | 8 | 2.133 | 1.077 | 3.265 | 55.116 | 0.780 |

- Separate PACE-only workload: one port per worker with fixed, evenly balanced persistent clients; no Asio/libuv comparison
- Clients, coordinator and server run on one host and contend for CPU; this is not an isolated server-capacity or linear-scaling estimate
- Changing workers also changes connections per reactor while total clients and requests stay fixed; workers are never inferred from CPU count
- All modes use the same coordinator plus one blocking thread per client; caller mode reduces spawned reactor threads, not the N active reactor threads
- Startup and steady-state windows are separate; warmup and startup are excluded from throughput, but client scheduling and join overhead are included
- TCP_NODELAY, pipeline=1 and loopback echo do not establish HTTP, accept balancing, remote-network or mixed-workload performance
- No CPU affinity or frequency controls are applied; run benchmarks sequentially on an otherwise idle host and inspect trial spread
- Source hashes describe files at runner invocation, not proof that a supplied binary was built from them; retain the compiler command with results
