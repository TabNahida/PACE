# PACE worker/execution scaling

3 trials per cell, 8 fixed clients, 64-byte messages, 5000 measured + 500 warmup requests/client.

PACE-only loopback echo with one listening port per worker. These results are separate from the Asio/libuv comparison.

| Mode | Workers | Requests/s median | Requests/s min-max | p50 us median | p99 us median |
|---|---:|---:|---:|---:|---:|
| runtime | 1 | 36,427 | 35,832-37,661 | 213.10 | 463.80 |
| caller | 1 | 37,162 | 36,574-37,537 | 211.90 | 433.00 |
| external | 1 | 37,128 | 36,981-37,139 | 210.90 | 444.80 |
| runtime | 2 | 53,096 | 52,181-53,405 | 114.40 | 596.00 |
| caller | 2 | 52,303 | 52,275-52,740 | 116.40 | 605.90 |
| external | 2 | 53,056 | 53,029-53,318 | 116.30 | 589.40 |
| runtime | 4 | 74,367 | 73,192-80,496 | 57.30 | 726.20 |
| caller | 4 | 77,276 | 76,172-77,334 | 56.00 | 780.60 |
| external | 4 | 77,644 | 77,330-79,403 | 56.00 | 785.40 |
| runtime | 8 | 86,430 | 75,450-87,104 | 38.50 | 173.50 |
| caller | 8 | 84,094 | 81,480-85,450 | 40.30 | 178.00 |
| external | 8 | 73,874 | 73,584-76,722 | 40.20 | 166.30 |

Lifecycle times below are medians in milliseconds; raw JSON also preserves every range and trial.

| Mode | Workers | Setup ms | Launch ms | Startup sum ms | Clients/warmup ms | Shutdown ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| runtime | 1 | 0.836 | 0.535 | 1.371 | 108.405 | 0.230 |
| caller | 1 | 0.852 | 0.004 | 0.856 | 109.009 | 0.156 |
| external | 1 | 0.870 | 0.477 | 1.313 | 106.867 | 0.221 |
| runtime | 2 | 1.035 | 0.590 | 1.625 | 50.142 | 0.348 |
| caller | 2 | 1.033 | 0.536 | 1.569 | 55.367 | 0.277 |
| external | 2 | 1.016 | 0.556 | 1.577 | 45.481 | 0.309 |
| runtime | 4 | 1.386 | 0.743 | 2.125 | 52.099 | 0.489 |
| caller | 4 | 1.399 | 0.703 | 2.155 | 53.646 | 0.471 |
| external | 4 | 1.467 | 0.800 | 2.268 | 53.913 | 0.473 |
| runtime | 8 | 2.200 | 1.055 | 3.313 | 58.394 | 0.797 |
| caller | 8 | 2.189 | 0.967 | 3.152 | 56.974 | 0.765 |
| external | 8 | 2.178 | 1.087 | 3.249 | 54.638 | 0.750 |

- Separate PACE-only workload: one port per worker with fixed, evenly balanced persistent clients; no Asio/libuv comparison
- Clients, coordinator and server run on one host and contend for CPU; this is not an isolated server-capacity or linear-scaling estimate
- Changing workers also changes connections per reactor while total clients and requests stay fixed; workers are never inferred from CPU count
- All modes use the same coordinator plus one blocking thread per client; caller mode reduces spawned reactor threads, not the N active reactor threads
- Startup and steady-state windows are separate; warmup and startup are excluded from throughput, but client scheduling and join overhead are included
- TCP_NODELAY, pipeline=1 and loopback echo do not establish HTTP, accept balancing, remote-network or mixed-workload performance
- No CPU affinity or frequency controls are applied; run benchmarks sequentially on an otherwise idle host and inspect trial spread
- Source hashes describe files at runner invocation, not proof that a supplied binary was built from them; retain the compiler command with results
