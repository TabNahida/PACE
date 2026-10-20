# PACE worker/execution scaling

5 trials per cell, 48 fixed clients, 64-byte messages, 10000 measured + 1000 warmup requests/client.

PACE-only loopback echo with one listening port per worker. These results are separate from the Asio/libuv comparison.

| Mode | Workers | Requests/s median | Requests/s min-max | p50 us median | p99 us median |
|---|---:|---:|---:|---:|---:|
| runtime | 1 | 314,390 | 309,000-317,656 | 147.85 | 232.38 |
| caller | 1 | 312,987 | 311,911-314,885 | 148.92 | 226.98 |
| external | 1 | 312,489 | 311,462-313,892 | 149.09 | 231.19 |
| runtime | 2 | 579,760 | 561,476-587,336 | 79.09 | 138.97 |
| caller | 2 | 583,887 | 553,375-592,158 | 78.67 | 137.58 |
| external | 2 | 585,225 | 546,546-592,477 | 79.06 | 133.69 |
| runtime | 4 | 813,434 | 730,916-821,596 | 48.13 | 171.86 |
| caller | 4 | 737,055 | 705,038-840,542 | 48.91 | 175.86 |
| external | 4 | 751,818 | 728,718-828,730 | 49.05 | 173.20 |
| runtime | 8 | 1,086,909 | 1,038,181-1,169,680 | 34.23 | 188.48 |
| caller | 8 | 1,041,005 | 1,020,440-1,095,517 | 35.02 | 186.18 |
| external | 8 | 1,060,943 | 930,654-1,153,863 | 34.35 | 189.88 |
| runtime | 24 | 2,380,521 | 1,116,543-2,609,039 | 8.39 | 59.18 |
| caller | 24 | 1,957,492 | 1,743,743-2,656,315 | 8.31 | 58.49 |
| external | 24 | 1,293,314 | 1,212,616-2,818,153 | 8.72 | 82.90 |
| runtime | 48 | 1,931,579 | 1,657,879-2,106,891 | 8.02 | 51.84 |
| caller | 48 | 1,734,904 | 1,575,720-2,202,200 | 7.30 | 52.61 |
| external | 48 | 1,994,361 | 1,832,141-2,229,562 | 7.69 | 51.35 |

Lifecycle times below are medians in milliseconds; raw JSON also preserves every range and trial.

| Mode | Workers | Setup ms | Launch ms | Startup sum ms | Clients/warmup ms | Shutdown ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| runtime | 1 | 0.030 | 0.052 | 0.081 | 188.098 | 0.121 |
| caller | 1 | 0.029 | 0.002 | 0.032 | 180.123 | 0.081 |
| external | 1 | 0.029 | 0.052 | 0.080 | 185.659 | 0.120 |
| runtime | 2 | 0.035 | 0.073 | 0.109 | 119.652 | 0.164 |
| caller | 2 | 0.037 | 0.056 | 0.093 | 121.350 | 0.145 |
| external | 2 | 0.035 | 0.067 | 0.102 | 122.599 | 0.159 |
| runtime | 4 | 0.045 | 0.146 | 0.190 | 92.123 | 0.204 |
| caller | 4 | 0.045 | 0.100 | 0.154 | 86.914 | 0.174 |
| external | 4 | 0.045 | 0.127 | 0.174 | 93.356 | 0.188 |
| runtime | 8 | 0.062 | 0.246 | 0.311 | 75.094 | 0.226 |
| caller | 8 | 0.060 | 0.232 | 0.292 | 70.666 | 0.202 |
| external | 8 | 0.065 | 0.232 | 0.299 | 83.397 | 0.204 |
| runtime | 24 | 0.135 | 0.579 | 0.709 | 60.848 | 0.341 |
| caller | 24 | 0.134 | 0.624 | 0.769 | 58.382 | 0.338 |
| external | 24 | 0.138 | 0.626 | 0.768 | 52.813 | 0.364 |
| runtime | 48 | 0.237 | 1.295 | 1.556 | 27.093 | 0.531 |
| caller | 48 | 0.246 | 1.208 | 1.430 | 25.753 | 0.528 |
| external | 48 | 0.250 | 1.329 | 1.578 | 28.931 | 0.551 |

- Separate PACE-only workload: one port per worker with fixed, evenly balanced persistent clients; no Asio/libuv comparison
- Clients, coordinator and server run on one host and contend for CPU; this is not an isolated server-capacity or linear-scaling estimate
- Changing workers also changes connections per reactor while total clients and requests stay fixed; workers are never inferred from CPU count
- All modes use the same coordinator plus one blocking thread per client; caller mode reduces spawned reactor threads, not the N active reactor threads
- Startup and steady-state windows are separate; warmup and startup are excluded from throughput, but client scheduling and join overhead are included
- TCP_NODELAY, pipeline=1 and loopback echo do not establish HTTP, accept balancing, remote-network or mixed-workload performance
- No CPU affinity or frequency controls are applied; run benchmarks sequentially on an otherwise idle host and inspect trial spread
- Source hashes describe files at runner invocation, not proof that a supplied binary was built from them; retain the compiler command with results
