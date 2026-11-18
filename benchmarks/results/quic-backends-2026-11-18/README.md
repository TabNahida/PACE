# QUIC transport on Linux loopback, 18 November 2026

Nine rounds per implementation and workload, with a fresh process per trial. Values are medians across rounds. RTT percentiles are computed within each trial before taking the median across trials.

| QUIC implementation | Bulk goodput (MiB/s) | RTT p50 (µs) | RTT p99 (µs) | Peak RSS, bulk (KiB) | Peak RSS, RTT (KiB) |
|---|---:|---:|---:|---:|---:|
| OpenSSL, 0.5.0 QUIC scheduler | 311.622 | 23.495 | 38.759 | 13020 | 11628 |
| OpenSSL, PACE scheduler | 312.944 | 25.856 | 40.699 | 12952 | 11512 |
| ngtcp2, PACE scheduler | 487.860 | 11.409 | 20.661 | 12224 | 11736 |

Switching scheduling within OpenSSL changes median goodput by +0.4%. The paired differences span −3.5% to +4.3%, so these runs do not establish a throughput gain. Short-message p50 rises 10.0% and p99 rises 5.0%. Peak bulk RSS changes by −68 KiB. Queuing continuations adds a scheduling step on this uncontended path. Its exact share of the latency change has not been isolated.

Relative to OpenSSL on the same PACE scheduler, ngtcp2 delivers 55.9% more application bytes per second, with 55.9% lower p50 and 49.2% lower p99 RTT. Bulk RSS falls by 728 KiB (5.6%), while short-message RSS rises by 224 KiB (1.9%).

## Workload

Both authenticated peers run in one process on one epoll reactor pinned to CPU 2. One established bidirectional QUIC stream carries application data over IPv4 loopback. Bulk sends 1 MiB of warm-up, then times 32 MiB in 16 KiB application writes through receipt of a one-byte confirmation. The receiver checks every byte. Latency times a 64-byte request and one-byte reply, one request outstanding, with 32 warm-ups and 1024 measured RTTs. Handshake is outside the timed interval. Peak RSS is the whole process's Linux VmHWM and includes both endpoints, TLS, initialization, and application buffers. These measurements cover single-stream local traffic. WAN RTT, loss, multiple connections and separate server memory require different workloads.

## Reproduction

The [probe](../../quic_transport.cpp) is available as the Linux xmake target `pace_quic_transport` under `--protocol_benchmarks=y --quic=y`. Run from the repository root so it can read the certificate fixtures. Select `--quic_backend=openssl` or `--quic_backend=ngtcp2`, build in release mode, and invoke the binary under `taskset -c 2` with `bulk` or `latency`. Each prints its metric(s), then peak RSS in KiB, as a CSV row.

For the controlled comparison we compiled the probe, src/runtime/*.cpp and src/networking/*.cpp once with GCC 13.3, C++20, -O3 -DNDEBUG -pthread -DPACE_HAS_QUIC. All three binaries link those exact objects. The old variant substitutes only `git show 3573b42:src/extensions/quic.cpp` for the new OpenSSL implementation. The third variant links quic_ngtcp2.cpp and libngtcp2_crypto_ossl/libngtcp2 1.26.0. All three use the same static OpenSSL 3.5.7. Host: Intel Core Ultra 7 270K Plus, WSL2 Linux 6.18.40.1, x86-64. The backend order rotates by one position each round. [Raw trials](trials.csv) include each RTT percentile, bulk rate and memory sample.
